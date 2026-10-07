// SHA-1 (RFC 3174). Нужен только для стабильных идентификаторов конфликтов: в Python они
// вычисляются как sha1("|".join(parts)).hexdigest()[:10], и C++ обязан дать те же строки.
#pragma once

#include <string>

namespace station {

std::string sha1_hex(const std::string& data);

}  // namespace station
