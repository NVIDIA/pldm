#pragma once
#include "libpldm/firmware_update.h"

#include "common/types.hpp"

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
constexpr size_t minimumSignatureSizeSha384 = 0x66;
constexpr size_t maximumSignatureSizeSha384 = 0x68;
const std::string packageSignatureSha384Name = "SHA384";

/** @brief Signature types length defined in PLDM firmware update specification
 */

constexpr uint8_t pldmFwupSignatureMagicLength = 4;
constexpr uint8_t pldmFwupSignaturePayloadLength = 4;
constexpr uint8_t pldmFwupSignatureLength = 2;

/** @struct PldmSignatureHeaderInformationV2
 *
 *  Structure representing fixed part of signature header information v2
 */
struct PldmSignatureHeaderInformationV2
{
    uint32_t magic;
    uint8_t majorVersion;
    uint8_t minorVersion;
    uint8_t securityVersion;
    uint16_t offsetToSignature;
    uint32_t payloadSize;
    uint8_t signatureType;
    uint16_t signatureSize;
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
        calculateDigest(std::istream& package, uintmax_t lengthOfSignedData);

  protected:
    std::string digestName;
    size_t chunkSize = 256;
};

/** @struct PackageSignatureSha384
 *
 *  Structure representing SHA384 hash function
 */
struct PackageSignatureSha384 : public PackageSignatureShaBase
{
  public:
    PackageSignatureSha384();
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
     *  @param[in] publicKey - Public Key
     */
    explicit PackageSignature(std::vector<uint8_t>& pkgSignData,
                              std::string publicKey) :
        packageSignData(pkgSignData),
        publicKey(publicKey)
    {}

    /** @brief Package signature verification function.
     *         Verify package using public key and signature stored
     *         in section Package Signature Header
     *
     *  @param[in] package - package to verify with signature
     *  @param[in] lengthOfSignedData - size of signed part of package
     *
     *  @return true if signature verification was successful,
     *            false if not
     */
    virtual bool verify(std::istream& package, uintmax_t lengthOfSignedData);

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
     *  @param[in] publicKey - Public Key
     *
     *  @return Concrete package signature parser
     */
    static std::unique_ptr<PackageSignature>
        createPackageSignatureParser(std::vector<uint8_t>& pkgSignData,
                                     const std::string& publicKey);

  protected:

    /** @brief SHA hash */
    std::unique_ptr<PackageSignatureShaBase> signatureSha;

    /** @brief Package Signature Header */
    std::vector<uint8_t> packageSignData;

    /** @brief Public Key */
    std::string publicKey;

    /** @brief Version of FW Update Package Signature Format */
    PackageSignatureVersion version;

    /** @brief Security version for the package */
    PackageSignatureSecurityVersion securityVersion;

    /** @brief Size of the FW Update Package being signed */
    PackageSignaturePayloadSize payloadSize;

    /** @brief Signature type */
    PackageSignatureSignatureType signatureType;

    /** @brief Size of the signature */
    PackageSignatureSignatureSize signatureSize;

    /** @brief Signature */
    PackageSignatureSignature signature;
};

class PackageSignatureV2 : public PackageSignature
{
  public:
    PackageSignatureV2() = delete;
    PackageSignatureV2(const PackageSignatureV2&) = delete;
    PackageSignatureV2& operator=(const PackageSignatureV2&) = delete;
    PackageSignatureV2(PackageSignatureV2&&) = default;
    PackageSignatureV2& operator=(PackageSignatureV2&&) = default;
    virtual ~PackageSignatureV2() = default;

    /** @brief Constructor
     *
     *  @param[in] pkgSignData - Package Signature Header with signature
     *  @param[in] publicKey - Public Key
     */
    explicit PackageSignatureV2(std::vector<uint8_t>& pkgSignData,
                                std::string publicKey) :
        PackageSignature(pkgSignData, publicKey)
    {
        signatureSha = std::make_unique<PackageSignatureSha384>();
    }

    PackageSignatureMinorVersion minorVersion;
    PackageSignatureOffsetToSignature offsetToSignature;

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
