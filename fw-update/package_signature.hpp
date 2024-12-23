/*
 * SPDX-FileCopyrightText: Copyright (c) 2021-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include "config.h"

#include "libpldm/firmware_update.h"

#include "common/types.hpp"
#include "common/utils.hpp"

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <phosphor-logging/lg2.hpp>
#include <sdeventplus/event.hpp>
#include <sdeventplus/source/event.hpp>

#include <array>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace pldm
{

namespace fw_update
{

constexpr size_t pldmFwupSignaturePackageSize = 1024;
constexpr PackageSignatureVersion packageSignatureVersion1 = 0x01;
constexpr PackageSignatureVersion packageSignatureVersion2 = 0x02;
constexpr PackageSignatureVersion packageSignatureVersion3 = 0x03;
constexpr size_t minimumSignatureSizeSha384 = 0x66;
constexpr size_t maximumSignatureSizeSha384 = 0x68;
const std::string packageSignatureSha384Name = "SHA384";

/** @brief Signature types length defined in PLDM firmware update specification
 */

constexpr uint8_t pldmFwupSignatureMagicLength = 4;
constexpr uint8_t pldmFwupSignaturePayloadLength = 4;
constexpr uint8_t pldmFwupSignatureSizeLength = 2;
constexpr uint8_t pldmFwupPublicKeySizeLength = 2;

/** @struct PldmSignatureHeaderInformationV3
 *
 *  Structure representing fixed part of signature header information v3
 */
struct PldmSignatureHeaderInformationV3
{
    uint32_t magic;
    uint8_t majorVersion;
    uint8_t minorVersion;
    uint8_t securityVersion;
    uint16_t offsetToSignature;
    uint32_t payloadSize;
    uint8_t signatureType;
    uint16_t offsetToPublicKey;
} __attribute__((packed));

/** @struct PackageSignatureShaBase
 *
 *  Base structure representing SHA hash function
 */
struct PackageSignatureShaBase
{
  public:
    PackageSignatureShaBase() = default;
    PackageSignatureShaBase(const PackageSignatureShaBase&) = delete;
    PackageSignatureShaBase& operator=(const PackageSignatureShaBase&) = delete;
    PackageSignatureShaBase(PackageSignatureShaBase&&) = default;
    PackageSignatureShaBase& operator=(PackageSignatureShaBase&&) = default;
    virtual ~PackageSignatureShaBase() = default;

    size_t minimumSignatureSize;
    size_t maximumSignatureSize;
    size_t digestLength;
    bool useChunks = true;

    /** @brief Calculate digest bases on concrete SHA algorithm.
     *
     *  @param[in] package - package to generate digest
     *  @param[in] lengthOfSignedData - size of signed part of package
     *
     *  @return digest
     */
    virtual std::vector<unsigned char>
        calculateDigest(std::istream& package,
                        uintmax_t lengthOfSignedData) = 0;

    /** @brief Asynchronously calculate digest based on a concrete SHA
     * algorithm.
     *
     *  This function performs the digest calculation asynchronously, allowing
     *  for non-blocking operations.
     *
     *  @param[in] package - package to generate digest
     *  @param[in] lengthOfSignedData - size of the signed part of the package
     *  @param[in] onComplete - callback to invoke upon successful digest
     * calculation, with the resulting digest as the argument
     *  @param[in] onError - callback to invoke if an error occurs during
     * calculation, with an exception message
     */
    virtual void calculateDigestAsync(
        std::istream& package, uintmax_t lengthOfSignedData,
        std::function<void(std::vector<unsigned char>)> onComplete,
        std::function<void(const std::string& errorMsg)> onError) = 0;

  protected:
    std::string digestName;
    size_t chunkSize = CALCULATE_DIGEST_CHUNK_SIZE;
    std::istream* package;
    std::shared_ptr<std::vector<unsigned char>> hash;
    int chunkNumber;
    std::shared_ptr<EVP_MD_CTX> ctxMdctxPtr;
    uintmax_t lengthOfSignedData;
    std::function<void(std::vector<unsigned char>)> onComplete;
    std::function<void(const std::string& errorMsg)> onError;

    /** @brief To send a request to handle chunk calculation */
    std::unique_ptr<sdeventplus::source::Defer> requestChunkCalculation;
};

/** @struct PackageSignatureSha384
 *
 *  Structure representing SHA384 hash function
 */
struct PackageSignatureSha384 : public PackageSignatureShaBase
{
  public:
    PackageSignatureSha384()
    {
        minimumSignatureSize = minimumSignatureSizeSha384;
        maximumSignatureSize = maximumSignatureSizeSha384;

        digestLength = SHA384_DIGEST_LENGTH;
        digestName = packageSignatureSha384Name;
        useChunks = true;
    }

    std::vector<unsigned char>
        calculateDigest(std::istream& package,
                        uintmax_t lengthOfSignedData) override;

    void calculateDigestAsync(
        std::istream& package, uintmax_t lengthOfSignedData,
        std::function<void(std::vector<unsigned char>)> onComplete,
        std::function<void(const std::string& errorMsg)> onError) override;

    void handleChunkProcessing(sd_event_source* source,
                               PackageSignatureShaBase* ctx);
};

/** @class PackageSignature
 *
 *  PackageSignature is the abstract base class for parsing the signature
 *  package header and verifing signature. Class inherited from the
 *  PackageSignature will implement schema of concrete version of signature
 *  header and verification method.
 */
class PackageSignature
{
  public:
    PackageSignature() = delete;
    PackageSignature(const PackageSignature&) = delete;
    PackageSignature& operator=(const PackageSignature&) = delete;
    PackageSignature(PackageSignature&&) = default;
    PackageSignature& operator=(PackageSignature&&) = default;
    virtual ~PackageSignature() = default;

    /** @brief Constructor
     *
     *  @param[in] pkgSignData - Package Signature Header with signature
     */
    explicit PackageSignature(std::vector<uint8_t>& pkgSignData) :
        packageSignData(pkgSignData)
    {}

    /** @brief Package signature verification function.
     *         Verify package using public key and signature stored
     *         in section Package Signature Header
     *
     *  @param[in] package - package to verify with signature
     *  @param[in] publicKey - Public Key
     *  @param[in] lengthOfSignedData - size of signed part of package
     *
     *  @return true if signature verification was successful, false if not
     */
    virtual bool verify(std::istream& package, const std::string& publicKey,
                        uintmax_t lengthOfSignedData);

    /** @brief Asynchronous package signature verification function.
     *
     *  This function performs the signature verification asynchronously,
     *  enabling non-blocking operations. The function invokes a callback
     *  upon completion or error, providing the result of the verification.
     *
     *  @param[in] package - The input stream containing the package to verify.
     *  @param[in] publicKey - The public key to use for signature verification.
     *  @param[in] lengthOfSignedData - The size of the signed portion of the
     * package.
     *  @param[in] onComplete - Callback to invoke with the verification result
     *                          (true if successful, false otherwise).
     *  @param[in] onError - Callback to invoke if an error occurs during
     * verification, providing an exception pointer for detailed error
     * information.
     */
    virtual void
        verifyAsync(std::istream& package, const std::string& publicKey,
                    uintmax_t lengthOfSignedData,
                    std::function<void(bool)> onComplete,
                    std::function<void(const std::string& errorMsg)> onError);

    /** @brief Package signature integrity check function.
     *         Verify package using public key and signature stored
     *         in section Package Signature Header
     *
     *  @param[in] package - package to verify with signature
     *  @param[in] lengthOfSignedData - size of signed part of package
     *
     *  @return true if integrity check was successful, false if not
     */
    virtual bool integrityCheck(std::istream& package,
                                uintmax_t lengthOfSignedData);

    /** @brief Asynchronous package signature integrity check function.
     *
     *  This function performs an integrity check on a package asynchronously.
     *  The operation validates the package's signature stored in the "Package
     *  Signature Header" section, ensuring content authenticity and integrity.
     *  The function uses callbacks to report the result or errors.
     *
     *  @param[in] package - The input stream containing the package to verify.
     *  @param[in] lengthOfSignedData - The size of the signed portion of the
     * package.
     *  @param[in] onComplete - Callback to invoke with the result of the
     * integrity check (true if successful, false otherwise).
     *  @param[in] onError - Callback to invoke if an error occurs during the
     * integrity check, providing an exception pointer for detailed error
     *                       information.
     */
    virtual void integrityCheckAsync(
        std::istream& package, uintmax_t lengthOfSignedData,
        std::function<void(bool)> onComplete,
        std::function<void(const std::string& errorMsg)> onError);

    /** @brief Calculate size of signed data
     *         The size contains size of package without size of signature
     *         and signature
     *
     *  @param[in] sizeOfPkgWithoutSignHdr - size of package without signature
     *                                        part
     *
     *  @return size of signed data
     */
    virtual uintmax_t
        calculateSizeOfSignedData(uintmax_t sizeOfPkgWithoutSignHdr) = 0;

    /** @brief Parse signature header
     *
     *  The implementation depends on concrete package signature version.
     */
    virtual void parseHeader() = 0;

    /** @brief Get signature from package
     *
     *  Before use the method, method 'parseHeader' must be run
     *  to fill all necessary fields from the signature header
     *
     *  @return signature
     */
    const PackageSignatureSignature& getSignature() const
    {
        return signature;
    }

    /** @brief Get Package Signature Header
     *
     *  @param[in] package - package with signature part
     *  @param[in] sizeOfPkgWithoutSignHdr - size of package without signature
     *  part
     *
     *  @return Package Signature Header as a vector
     */
    static std::vector<uint8_t>
        getSignatureHeader(std::istream& package,
                           uintmax_t sizeOfPkgWithoutSignHdr);

    /** @brief Get Version of Package Signature Format
     *
     *  @param[in] pkgSignData - Package Signature Header
     *
     *  @return Version of Package Signature Format
     */
    static PackageSignatureVersion
        getSignatureVersion(std::vector<uint8_t>& pkgSignData);

    /** @brief Create parser for concrete Version of Package Signature Format
     *
     *  @param[in] pkgSignData - Package Signature Header
     *
     *  @return Concrete package signature parser
     */
    static std::unique_ptr<PackageSignature>
        createPackageSignatureParser(std::vector<uint8_t>& pkgSignData);

  protected:
    /** @brief SHA hash */
    std::unique_ptr<PackageSignatureShaBase> signatureSha;

    /** @brief Package Signature Header */
    std::vector<uint8_t> packageSignData;

    /** @brief Version of FW Update Package Signature Format */
    PackageSignatureVersion version = 0;

    /** @brief Minor Version of FW Update Package Signature Format */
    PackageSignatureMinorVersion minorVersion;

    /** @brief Security version for the package */
    PackageSignatureSecurityVersion securityVersion = 0;

    /** @brief offset to the signature */
    PackageSignatureOffsetToSignature offsetToSignature;

    /** @brief Size of the FW Update Package being signed */
    PackageSignaturePayloadSize payloadSize = 0;

    /** @brief Signature type */
    PackageSignatureSignatureType signatureType = 0;

    /** @brief offset to the public key */
    PackageSignatureOffsetToPublicKey offsetToPublicKey;

    /** @brief Size of the signature */
    PackageSignatureSignatureSize signatureSize = 0;

    /** @brief Signature */
    PackageSignatureSignature signature;

    /** @brief Size of the public key */
    PackageSignaturePublicKeySize publicKeySize;

    /** @brief Public key */
    PackageSignaturePublicKey publicKeyData;
};

class PackageSignatureV3 : public PackageSignature
{
  public:
    PackageSignatureV3() = delete;
    PackageSignatureV3(const PackageSignatureV3&) = delete;
    PackageSignatureV3& operator=(const PackageSignatureV3&) = delete;
    PackageSignatureV3(PackageSignatureV3&&) = default;
    PackageSignatureV3& operator=(PackageSignatureV3&&) = default;
    virtual ~PackageSignatureV3() = default;

    /** @brief Constructor
     *
     *  @param[in] pkgSignData - Package Signature Header with signature
     */
    explicit PackageSignatureV3(std::vector<uint8_t>& pkgSignData) :
        PackageSignature(pkgSignData)
    {
        signatureSha = std::make_unique<PackageSignatureSha384>();
    }

    PackageSignatureMinorVersion minorVersion = 0;
    PackageSignatureOffsetToSignature offsetToSignature = 0;

    /** @brief Calculate size of signed data
     *         The size contains size of package without size of signature
     *         and signature
     *
     *  @param[in] sizeOfPkgWithoutSignHdr - size of package without signature
     * part
     *
     *  @return size of signed data
     */
    virtual uintmax_t
        calculateSizeOfSignedData(uintmax_t sizeOfPkgWithoutSignHdr);

    /** @brief Parse signature header
     *
     *  The method verify signature header and reads particular fields
     */
    virtual void parseHeader();
};

} // namespace fw_update

} // namespace pldm
