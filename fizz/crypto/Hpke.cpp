/*
 *  Copyright (c) 2019-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree.
 */

#include <fizz/crypto/Hpke.h>
#include <fizz/crypto/HpkeTypes.h>

namespace fizz {
namespace hpke {

std::unique_ptr<folly::IOBuf> writeKeyScheduleContext(Mode mode, const std::vector<uint8_t> &pskIdHash,
  const std::vector<uint8_t> &infoHash) {
  std::unique_ptr<folly::IOBuf> keyScheduleContext = folly::IOBuf::create(0);
  folly::io::Appender appender(keyScheduleContext.get(), 20);
  detail::write<uint8_t>((uint8_t) mode, appender);

  detail::writeBufWithoutLength(folly::IOBuf::copyBuffer(pskIdHash), appender);
  detail::writeBufWithoutLength(folly::IOBuf::copyBuffer(infoHash), appender);
  return keyScheduleContext;
}

HpkeContext keySchedule(KeyScheduleParams params) {
  auto hkdf = std::move(params.hkdf);

  auto psk = params.pskInputs.hasValue() ? std::move(params.pskInputs.value().psk) : PskInputs::defaultPsk->clone();
  auto pskId = params.pskInputs.hasValue() ? std::move(params.pskInputs.value().id) : PskInputs::defaultId->clone();

  // Generate hashes for key schedule context
  std::vector<uint8_t> pskIdHash = hkdf->labeledExtract(folly::IOBuf::copyBuffer(""), folly::Range("psk_id_hash"),
    std::move(pskId), params.suiteId->clone());
  std::vector<uint8_t> infoHash = hkdf->labeledExtract(folly::IOBuf::copyBuffer(""), folly::Range("info_hash"),
    std::move(params.info), params.suiteId->clone());
  std::unique_ptr<folly::IOBuf> keyScheduleContext = writeKeyScheduleContext(params.mode, pskIdHash, infoHash);

  // Generate hashes for cipher key
  std::vector<uint8_t> pskHash = hkdf->labeledExtract(folly::IOBuf::copyBuffer(""), folly::Range("psk_hash"), std::move(psk),
    params.suiteId->clone());
  std::vector<uint8_t> secret = hkdf->labeledExtract(folly::IOBuf::copyBuffer(pskHash), folly::Range("secret"), std::move(params.sharedSecret),
    params.suiteId->clone());

  // Generate additional values needed for contructing context
  std::unique_ptr<folly::IOBuf> key = hkdf->labeledExpand(secret, folly::Range("key"), keyScheduleContext->clone(), params.cipher->keyLength(),
    params.suiteId->clone());
  std::unique_ptr<folly::IOBuf> nonce = hkdf->labeledExpand(secret, folly::Range("nonce"), keyScheduleContext->clone(), params.cipher->ivLength(),
    params.suiteId->clone());
  std::unique_ptr<folly::IOBuf> exporterSecret = hkdf->labeledExpand(secret, folly::Range("exp"), std::move(keyScheduleContext),
    hkdf->hashLength(), params.suiteId->clone());

  // Configure cipher to use our generated key
  TrafficKey trafficKey;
  trafficKey.key = std::move(key);
  trafficKey.iv = std::move(nonce);
  params.cipher->setKey(std::move(trafficKey));

  return HpkeContext(std::move(params.cipher), std::move(exporterSecret), std::move(hkdf), std::move(params.suiteId));
}

SetupResult setupWithEncap(Mode mode, folly::ByteRange pkR, std::unique_ptr<folly::IOBuf> info, folly::Optional<PskInputs> pskInputs, SetupParam param) {
  DHKEM::EncapResult encapResult = param.dhkem->encap(pkR);

  KeyScheduleParams keyScheduleParams{mode, std::move(encapResult.sharedSecret), std::move(info), std::move(pskInputs),
    std::move(param.cipher), std::move(param.hkdf), std::move(param.suiteId)};

  SetupResult result{std::move(encapResult.enc), keySchedule(std::move(keyScheduleParams))};
  return result;
}

HpkeContext setupWithDecap(Mode mode, folly::ByteRange enc, std::unique_ptr<folly::IOBuf> info, folly::Optional<PskInputs> pskInputs, SetupParam param) {
  auto sharedSecret = param.dhkem->decap(enc);
  KeyScheduleParams keyScheduleParams{mode, std::move(sharedSecret), std::move(info), std::move(pskInputs),
    std::move(param.cipher), std::move(param.hkdf), std::move(param.suiteId)};

  return keySchedule(std::move(keyScheduleParams));
}

} // namespace hpke
} // namespace fizz
