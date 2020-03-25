// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////////

#include "tink/subtle/prf/hkdf_streaming_prf.h"

#include <algorithm>
#include <utility>

#include "absl/memory/memory.h"
#include "absl/strings/str_cat.h"
#include "openssl/base.h"
#include "openssl/hkdf.h"
#include "openssl/hmac.h"
#include "tink/subtle/subtle_util.h"
#include "tink/subtle/subtle_util_boringssl.h"
#include "tink/util/secret_data.h"

namespace crypto {
namespace tink {
namespace subtle {

namespace {

class HkdfInputStream : public InputStream {
 public:
  HkdfInputStream(const EVP_MD *digest, const util::SecretData &secret,
                  absl::string_view salt, absl::string_view input)
      : digest_(digest), input_(input) {
    prk_.resize(EVP_MAX_MD_SIZE);
    size_t prk_len;

    if (1 != HKDF_extract(
                 prk_.data(), &prk_len, digest_, secret.data(), secret.size(),
                 reinterpret_cast<const uint8_t *>(salt.data()), salt.size())) {
      stream_status_ =
          util::Status(util::error::INTERNAL, "BoringSSL's HKDF failed");
    }
    prk_.resize(prk_len);
  }

  crypto::tink::util::StatusOr<int> Next(const void **data) override {
    if (!stream_status_.ok()) {
      return stream_status_;
    }
    if (position_in_ti_ < ti_.size()) {
      return returnDataFromPosition(data);
    }
    if (i_ == 255) {
      stream_status_ = crypto::tink::util::Status(
          crypto::tink::util::error::OUT_OF_RANGE, "EOF");
      return stream_status_;
    }
    stream_status_ = UpdateTi();
    if (!stream_status_.ok()) {
      return stream_status_;
    }
    return returnDataFromPosition(data);
  }

  void BackUp(int count) override {
    position_in_ti_ -= std::min(std::max(0, count), position_in_ti_);
  }

  int64_t Position() const override {
    if (i_ == 0) return 0;
    return (i_ - 1) * EVP_MD_size(digest_) + position_in_ti_;
  }

 private:
  int returnDataFromPosition(const void **data) {
    // There's still data in ti to return.
    *data = &ti_[position_in_ti_];
    int result = ti_.size() - position_in_ti_;
    position_in_ti_ = ti_.size();
    return result;
  }

  // Sets T(i+i) = HMAC-Hash(PRK, T(i) | info | i + 1) as in RFC 5869,
  // Section 2.3
  // Unfortunately, boringSSL does not provide a function which updates T(i)
  // for a single round; hence we implement this ourselves.
  util::Status UpdateTi() {
    HMAC_CTX hmac_ctx;
    HMAC_CTX_init(&hmac_ctx);

    if (!HMAC_Init_ex(&hmac_ctx, reinterpret_cast<const uint8_t *>(prk_.data()),
                      prk_.size(), digest_, nullptr)) {
      HMAC_CTX_cleanup(&hmac_ctx);
      return util::Status(util::error::INTERNAL,
                          "BoringSSL's HMAC_Init_ex failed");
    }
    if (!HMAC_Update(&hmac_ctx, ti_.data(), ti_.size())) {
      HMAC_CTX_cleanup(&hmac_ctx);
      return util::Status(util::error::INTERNAL,
                          "BoringSSL's HMAC_Update failed on ti_");
    }
    if (!HMAC_Update(&hmac_ctx, reinterpret_cast<const uint8_t *>(&input_[0]),
                     input_.size())) {
      HMAC_CTX_cleanup(&hmac_ctx);
      return util::Status(util::error::INTERNAL,
                          "BoringSSL's HMAC_Update failed on input_");
    }
    uint8_t i_as_uint8 = i_ + 1;
    if (!HMAC_Update(&hmac_ctx, &i_as_uint8, 1)) {
      HMAC_CTX_cleanup(&hmac_ctx);
      return util::Status(util::error::INTERNAL,
                          "BoringSSL's HMAC_Update failed on i_");
    }
    ti_.resize(EVP_MD_size(digest_));
    if (!HMAC_Final(&hmac_ctx, ti_.data(), nullptr)) {
      HMAC_CTX_cleanup(&hmac_ctx);
      return util::Status(util::error::INTERNAL,
                          "BoringSSL's HMAC_Final failed");
    }
    HMAC_CTX_cleanup(&hmac_ctx);
    i_++;
    position_in_ti_ = 0;
    return util::OkStatus();
  }

  // OUT_OF_RANGE_ERROR in case we returned all the data. Other errors indicate
  // problems and are permanent.
  util::Status stream_status_ = util::OkStatus();

  const EVP_MD *digest_;

  // PRK as by RFC 5869, Section 2.2
  util::SecretData prk_;

  // Current value T(i).
  util::SecretData ti_;
  // By RFC 5869: 0 <= i_ <= 255*HashLen
  int i_ = 0;

  std::string input_;

  // The current position of ti which we returned.
  int position_in_ti_ = 0;
};

}  // namespace

std::unique_ptr<InputStream> HkdfStreamingPrf::ComputePrf(
    absl::string_view input) const {
  return absl::make_unique<HkdfInputStream>(hash_, secret_, salt_, input);
}

// static
crypto::tink::util::StatusOr<std::unique_ptr<StreamingPrf>>
HkdfStreamingPrf::New(HashType hash, util::SecretData secret,
                      absl::string_view salt) {
  if (hash != SHA256 && hash != SHA512 && hash != SHA1) {
    return util::Status(
        util::error::INVALID_ARGUMENT,
        absl::StrCat("Hash ", hash, " not acceptable for HkdfStreamingPrf"));
  }

  if (secret.size() < 10) {
    return util::Status(util::error::INVALID_ARGUMENT,
                        "Too short secret for HkdfStreamingPrf");
  }
  auto evp_md_or = SubtleUtilBoringSSL::EvpHash(hash);
  if (!evp_md_or.ok()) {
    return util::Status(util::error::UNIMPLEMENTED, "Unsupported hash");
  }

  return {absl::WrapUnique(
      new HkdfStreamingPrf(evp_md_or.ValueOrDie(), std::move(secret), salt))};
}

}  // namespace subtle
}  // namespace tink
}  // namespace crypto
