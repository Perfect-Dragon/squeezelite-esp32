#include "CDNAudioFile.h"

#include <string.h>          // for memcpy
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

CDNAudioFile::CDNAudioFile(const std::string& cdnUrl,
                           const std::vector<uint8_t>& audioKey)
    : cdnUrl(cdnUrl), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
}

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  this->enableRequestMargin = true;
  this->position = newPos;
}

bool CDNAudioFile::fetchHttpRange(
    const bell::HTTPClient::ValueHeader& range, uint8_t* dst,
    size_t dstCapacity, size_t& readCapacity, const char* label,
    size_t logPosition) {
  constexpr int HTTP_ATTEMPTS = 2;

  readCapacity = 0;

  for (int attempt = 0; attempt < HTTP_ATTEMPTS; ++attempt) {
    try {
      if (!this->httpConnection) {
        this->httpConnection =
            std::make_unique<bell::HTTPClient::Response>();
        this->httpConnection->connect(this->cdnUrl);
      }

      auto httpStart = bell::tv::now().ms();

      CSPOT_LOG(info, "CDN %s begin response=%p pos=%u attempt=%d",
                label, static_cast<void*>(httpConnection.get()),
                (unsigned)logPosition, attempt + 1);

      this->httpConnection->get(this->cdnUrl, {range});

      auto headersDone = bell::tv::now().ms();

      readCapacity = this->httpConnection->contentLength();

      if (readCapacity == 0 || readCapacity > dstCapacity) {
        throw std::runtime_error("Invalid HTTP audio range length");
      }

      CSPOT_LOG(info, "CDN %s body begin response=%p pos=%u expected=%u",
                label, static_cast<void*>(httpConnection.get()),
                (unsigned)logPosition, (unsigned)readCapacity);

      this->httpConnection->stream().read((char*)dst, readCapacity);

      auto bodyDone = bell::tv::now().ms();

      auto headerMs = headersDone - httpStart;
      auto bodyMs = bodyDone - headersDone;
      auto totalMs = bodyDone - httpStart;

      CSPOT_LOG(info, "CDN %s body end response=%p got=%lld expected=%u state=%u",
                label, static_cast<void*>(httpConnection.get()),
                (long long)httpConnection->stream().gcount(),
                (unsigned)readCapacity,
                (unsigned)httpConnection->stream().rdstate());

      if (totalMs > 100 || attempt > 0) {
        CSPOT_LOG(info,
                  "CDN %s pos=%u size=%u attempt=%d headers=%lldms "
                  "body=%lldms total=%lldms",
                  label, (unsigned int)logPosition,
                  (unsigned int)readCapacity, attempt + 1,
                  (long long)headerMs, (long long)bodyMs,
                  (long long)totalMs);
      }

      if (this->httpConnection->stream().gcount() !=
          (std::streamsize)readCapacity) {
        throw std::runtime_error("Short read of audio range body");
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

      if (attempt + 1 < HTTP_ATTEMPTS) {
        BELL_SLEEP_MS(50);
      }
    }
  }

  return false;
}

void CDNAudioFile::openStream() {
  CSPOT_LOG(info, "Opening HTTP stream to %s", this->cdnUrl.c_str());

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

  // Location must be dividable by 16
  size_t footerStartLocation =
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;

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
}

size_t CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  size_t offsetPosition = position + SPOTIFY_OPUS_HEADER;
  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;

  if (position + bytes >= this->totalFileSize) {
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

    size_t readCapacity = 0;

    if (!fetchHttpRange(
            bell::HTTPClient::RangeHeader::range(
                requestPosition, requestPosition + HTTP_BUFFER_SIZE - 1),
            this->httpBuffer.data(), this->httpBuffer.size(),
            readCapacity, "READ", requestPosition)) {
      // Do not throw through libvorbis' C callbacks. After one reconnect
      // and retry of the same byte range, report EOF and let TrackPlayer
      // unwind cleanly.
      CSPOT_LOG(error, "Failed to read audio chunk at %u after retry",
                (unsigned int)requestPosition);
      this->lastRequestPosition = 0;
      this->lastRequestCapacity = 0;
      return 0;
    }

    this->lastRequestPosition = requestPosition;
    this->lastRequestCapacity = readCapacity;

    this->decrypt(this->httpBuffer.data(), lastRequestCapacity,

                  this->lastRequestPosition);

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
