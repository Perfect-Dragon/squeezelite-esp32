#include "CDNAudioFile.h"

#include <string.h>          // for memcpy
#include <algorithm>         // for min
#include <functional>        // for __base
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <stdexcept>         // for runtime_error
#include <string_view>       // for string_view
#include <type_traits>       // for remove_extent_t
#include "BellUtils.h"

#include "AccessKeyFetcher.h"  // for AccessKeyFetcher
#include "BellLogger.h"        // for AbstractLogger
#include "Crypto.h"
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "SocketStream.h"      // for SocketStream
#include "Utils.h"             // for bigNumAdd, bytesToHexString, string...
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

CDNAudioFile::CDNAudioFile(const std::vector<std::string>& cdnUrls,
                           const std::vector<uint8_t>& audioKey)
    : cdnUrls(cdnUrls), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
}

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  this->enableRequestMargin = true;
  this->shortNextRequest = true;
  this->position = newPos;
}

void CDNAudioFile::rewind() {
  this->aborted = false;
  this->enableRequestMargin = false;
  this->shortNextRequest = true;
  this->position = 0;
}

bool CDNAudioFile::fetchHttpRange(
    const bell::HTTPClient::ValueHeader& range, uint8_t* dst,
    size_t dstCapacity, size_t& readCapacity, const char* label,
    size_t logPosition) {
  constexpr int HTTP_ATTEMPTS = 5;

  readCapacity = 0;

  for (int attempt = 0; attempt < HTTP_ATTEMPTS && !this->aborted; ++attempt) {
    const std::string& cdnUrl = this->cdnUrls[this->urlIndex];
    bool connectFailed = false;

    try {
      if (!this->httpConnection) {
        this->httpConnection =
            std::make_unique<bell::HTTPClient::Response>();
        // a failure here is retried below, possibly on another CDN host
        this->httpConnection->reconnectRetry = false;
        connectFailed = true;
        this->httpConnection->connect(cdnUrl);
        connectFailed = false;
      }

      this->httpConnection->get(cdnUrl, {range});

      readCapacity = this->httpConnection->contentLength();

      if (readCapacity == 0 || readCapacity > dstCapacity) {
        throw std::runtime_error("Invalid HTTP audio range length");
      }

      // On a slow link a whole range can take many seconds; read it in pieces
      // so that a skip (abort()) does not have to wait for all of it
      for (size_t got = 0; got < readCapacity;) {
        if (this->aborted) {
          throw std::runtime_error("aborted");
        }

        const size_t piece = std::min(readCapacity - got, (size_t)ABORT_CHECK_SIZE);
        this->httpConnection->stream().read((char*)dst + got, piece);

        if (this->httpConnection->stream().gcount() != (std::streamsize)piece) {
          throw std::runtime_error("Short read of audio range body");
        }
        got += piece;
      }

      return true;
    } catch (const std::exception& e) {
      CSPOT_LOG(error,
                "CDN %s pos=%u attempt=%d/%d failed: %s",
                label, (unsigned int)logPosition, attempt + 1,
                HTTP_ATTEMPTS, e.what());

      // Drop the entire HTTP/TLS object. A timed-out keep-alive
      // connection must not be reused for the retry.
      this->httpConnection.reset();
      readCapacity = 0;

      // Only an unreachable host is worth leaving. One that accepted the
      // connection but is slow to answer is usually filling its cache: the
      // end of a song it does not have yet comes only after it fetched the
      // whole file (seen: 4-11 s), and another host would start that over.
      if (connectFailed && this->cdnUrls.size() > 1) {
        this->urlIndex = (this->urlIndex + 1) % this->cdnUrls.size();
        CSPOT_LOG(info, "Switching to CDN host %u/%u",
                  (unsigned)this->urlIndex + 1, (unsigned)this->cdnUrls.size());
      }

      if (attempt + 1 < HTTP_ATTEMPTS) {
        BELL_SLEEP_MS(50);
      }
    }
  }

  return false;
}

void CDNAudioFile::openStream() {
  if (this->opened) {
    CSPOT_LOG(info, "HTTP stream already open");
    return;
  }

  CSPOT_LOG(info, "Opening HTTP stream to %s", this->cdnUrls[this->urlIndex].c_str());

  size_t headerCapacity = 0;

  if (!fetchHttpRange(
          bell::HTTPClient::RangeHeader::range(0, OPUS_HEADER_SIZE - 1),
          this->header.data(), this->header.size(), headerCapacity,
          "HEADER", 0) ||
      headerCapacity != this->header.size()) {
    throw std::runtime_error("Failed to fetch complete audio header");
  }

  this->totalFileSize =
      this->httpConnection->totalLength() - SPOTIFY_OPUS_HEADER;

  this->decrypt(header.data(), OPUS_HEADER_SIZE, 0);

  // Location must be dividable by 16 (whole file for very short tracks)
  size_t footerStartLocation = 0;
  if (this->totalFileSize + SPOTIFY_OPUS_HEADER > (size_t)OPUS_FOOTER_PREFFERED) {
    footerStartLocation =
        (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
        (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;
  }

  this->footer = std::vector<uint8_t>(
      this->totalFileSize - footerStartLocation + SPOTIFY_OPUS_HEADER);

  size_t footerCapacity = 0;

  if (!fetchHttpRange(
          bell::HTTPClient::RangeHeader::last(this->footer.size()),
          this->footer.data(), this->footer.size(), footerCapacity,
          "FOOTER", footerStartLocation) ||
      footerCapacity != this->footer.size()) {
    throw std::runtime_error("Failed to fetch complete audio footer");
  }

  this->decrypt(footer.data(), footer.size(), footerStartLocation);
  CSPOT_LOG(info, "Header and footer bytes received");
  this->position = 0;
  this->lastRequestPosition = 0;
  this->lastRequestCapacity = 0;
  this->opened = true;
}

void CDNAudioFile::prefetch() {
  openStream();

  if (this->lastRequestCapacity == 0 && fetchChunk(0, HTTP_BUFFER_SIZE)) {
    CSPOT_LOG(info, "Prefetched %u bytes", (unsigned)this->lastRequestCapacity);
  }

  // Nothing else will be read until playback reaches the end of this window,
  // so don't hold a TLS session (and its memory) open for it meanwhile
  this->httpConnection.reset();
}

bool CDNAudioFile::fetchChunk(size_t requestPosition, size_t size) {
  size_t readCapacity = 0;

  CSPOT_LOG(info, "CDN fetch pos=%u size=%u", (unsigned)requestPosition,
            (unsigned)size);

  if (!fetchHttpRange(bell::HTTPClient::RangeHeader::range(
                          requestPosition, requestPosition + size - 1),
                      this->httpBuffer.data(), this->httpBuffer.size(),
                      readCapacity, "READ", requestPosition)) {
    this->lastRequestPosition = 0;
    this->lastRequestCapacity = 0;
    return false;
  }

  this->lastRequestPosition = requestPosition;
  this->lastRequestCapacity = readCapacity;
  this->decrypt(this->httpBuffer.data(), readCapacity, requestPosition);
  return true;
}

size_t CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  size_t offsetPosition = position + SPOTIFY_OPUS_HEADER;
  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;

  // a read reaching the end is truncated below (footer), only report EOF at the end
  if (position >= this->totalFileSize) {
    return 0;
  }

  // // Opus tries to read header, use prefetched data
  if (offsetPosition < OPUS_HEADER_SIZE &&
      bytes + offsetPosition <= OPUS_HEADER_SIZE) {
    memcpy(dst, this->header.data() + offsetPosition, bytes);
    position += bytes;
    return bytes;
  }

  // // Opus tries to read footer, use prefetched data
  if (offsetPosition >= (actualFileSize - this->footer.size())) {
    size_t toReadBytes = bytes;

    if ((position + bytes) > this->totalFileSize) {
      // Tries to read outside of bounds, truncate
      toReadBytes = this->totalFileSize - position;
    }

    size_t footerOffset =
        offsetPosition - (actualFileSize - this->footer.size());
    memcpy(dst, this->footer.data() + footerOffset, toReadBytes);

    position += toReadBytes;
    return toReadBytes;
  }

  // Data not in the headers. Make sense of whats going on.
  // Position in bounds :)
  if (offsetPosition >= this->lastRequestPosition &&
      offsetPosition < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + offsetPosition) >
        this->lastRequestPosition + lastRequestCapacity) {
      toRead = this->lastRequestPosition + lastRequestCapacity - offsetPosition;
    }

    memcpy(dst, this->httpBuffer.data() + offsetPosition - lastRequestPosition,
           toRead);
    position += toRead;

    return toRead;
  } else {
    size_t requestPosition = (offsetPosition) - ((offsetPosition) % 16);
    if (this->enableRequestMargin && requestPosition > SEEK_MARGIN_SIZE) {
      requestPosition = (offsetPosition - SEEK_MARGIN_SIZE) -
                        ((offsetPosition - SEEK_MARGIN_SIZE) % 16);
      this->enableRequestMargin = false;
    }

    const size_t requestSize =
        this->shortNextRequest ? HTTP_FIRST_CHUNK_SIZE : HTTP_BUFFER_SIZE;
    this->shortNextRequest = false;

    if (!fetchChunk(requestPosition, requestSize)) {
      // Do not throw through libvorbis' C callbacks. After one reconnect
      // and retry of the same byte range, report EOF and let TrackPlayer
      // unwind cleanly.
      CSPOT_LOG(error, "Failed to read audio chunk at %u after retry",
                (unsigned int)requestPosition);
      return 0;
    }

    return readBytes(dst, bytes);
  }

  return bytes;
}

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);

  this->crypto->aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}
