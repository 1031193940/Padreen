#pragma once
// Taking the driver files out of Parsec's installers without any third-party tool.
// Parsec publishes the virtual display driver as NSIS installers (https://builds.parsec.app/vdd/...).  Their data is a sequence of blobs, each
// compressed on its own with plain DEFLATE.  Nothing here trusts the archive: a file is chosen by its SHA-256 (never by a name stored in the
// archive), and every length that comes from the archive is checked against the data that is really there.
#include "crypto.h"

namespace ph {

// Raw DEFLATE (RFC 1951).  false = malformed or truncated input, or more than maxOut bytes of output.
bool inflateRaw(const uint8_t* in, size_t inLen, size_t maxOut, Bytes& out);

// Looks for files in an NSIS installer by content: every data blob is unpacked and its SHA-256 (lower case hex) is compared with the wanted
// values.  found[i] gets the file whose hash is wanted[i].  true = all of them were found.
bool nsisFindFiles(const Bytes& installer, const std::vector<std::string>& wantedSha256, std::vector<Bytes>& found);

}  // namespace ph
