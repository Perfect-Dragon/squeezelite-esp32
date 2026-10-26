#pragma once

#include <atomic>   // for atomic
#include <cstddef>  // for size_t
#include <cstdint>  // for uint8_t
#include <memory>   // for shared_ptr, unique_ptr
#include <string>   // for string
#include <vector>   // for vector

#include "Crypto.h"      // for Crypto
#include "HTTPClient.h"  // for HTTPClient

namespace bell {
class WrappedSemaphore;
}  // namespace bell

namespace cspot {
class AccessKeyFetcher;

class CDNAudioFile {

 public:
  CDNAudioFile(const std::string& cdnUrl, const std::vector<uint8_t>& audioKey);

  /**
  * @brief Opens connection to the provided cdn url, and fetches track metadata.
  * Does nothing if the stream is already open.
  */
  void openStream();

  /**
  * @brief Fills the read-ahead window from the start of the file, then
  * releases the connection. Lets a queued track start without network I/O.
  */
  void prefetch();

  /**
  * @brief Moves back to the start, keeping header, footer and read-ahead data.
  * Also clears abort().
  */
  void rewind();

  /**
  * @brief Makes a download in progress (on another thread) give up within one
  * ABORT_CHECK_SIZE piece, and further reads fail, until rewind()
  */
  void abort() { aborted = true; }

  /**
  * @brief Read and decrypt part of the cdn stream
  *
  * @param dst buffer where to read received data to
  * @param amount of bytes to read
  *
  * @returns amount of bytes read
  */
  size_t readBytes(uint8_t* dst, size_t bytes);

  /**
  * @brief Returns current position in CDN stream
  */
  size_t getPosition();

  /**
  * @brief returns total size of the audio file in bytes
  */
  size_t getSize();

  /**
  * @brief Seeks the track to provided position
  * @param position position where to seek the track
  */
  void seek(size_t position);

 private:
  const int OPUS_HEADER_SIZE = 8 * 1024;
  // ov_open() (codecs' libvorbisidec) looks for the last page by reading from
  // CHUNKSIZE (65535) bytes before the end, so cache all of that: otherwise
  // opening a track costs an extra request that also evicts the read-ahead window
  const int OPUS_FOOTER_PREFFERED = 1024 * 64;
  const int SEEK_MARGIN_SIZE = 1024 * 4;

  // Keep a sequential read-ahead window so normal playback does not block on
  // a new TLS/HTTP request at short compressed-audio intervals. Seeking still
  // invalidates this cache through enableRequestMargin and the existing range path.
  const int HTTP_BUFFER_SIZE = 1024 * 256;
  // First request after opening or seeking is smaller so decoding starts sooner
  const int HTTP_FIRST_CHUNK_SIZE = 1024 * 128;
  // Bodies are read in pieces of this size, checking for abort() in between
  const int ABORT_CHECK_SIZE = 1024 * 16;
  const int SPOTIFY_OPUS_HEADER = 167;

  // Used to store opus metadata, speeds up read
  std::vector<uint8_t> header = std::vector<uint8_t>(OPUS_HEADER_SIZE);
  std::vector<uint8_t> footer;

  // General purpose buffer to read data
  std::vector<uint8_t> httpBuffer = std::vector<uint8_t>(HTTP_BUFFER_SIZE);

  // AES IV for decrypting the audio stream
  const std::vector<uint8_t> audioAESIV = {0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb,
                                           0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64,
                                           0x3f, 0x63, 0x0d, 0x93};
  std::unique_ptr<Crypto> crypto;

  std::unique_ptr<bell::HTTPClient::Response> httpConnection;

  size_t position = 0;
  size_t totalFileSize = 0;
  size_t lastRequestPosition = 0;
  size_t lastRequestCapacity = 0;

  bool enableRequestMargin = false;
  bool opened = false;
  bool shortNextRequest = true;
  std::atomic<bool> aborted = false;

  std::string cdnUrl;
  std::vector<uint8_t> audioKey;

  bool fetchHttpRange(const bell::HTTPClient::ValueHeader& range,
                      uint8_t* dst, size_t dstCapacity,
                      size_t& readCapacity, const char* label,
                      size_t logPosition);
  bool fetchChunk(size_t requestPosition, size_t size);
  void decrypt(uint8_t* dst, size_t nbytes, size_t pos);
};
}  // namespace cspot
