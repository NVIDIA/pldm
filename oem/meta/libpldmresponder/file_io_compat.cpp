#include <libpldm/base.h>
#include <libpldm/oem/meta/file_io.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <endian.h>

extern "C"
{

void* pldm_oem_meta_file_io_write_req_data(
    struct pldm_oem_meta_file_io_write_req* req)
{
    if (req == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<uint8_t*>(req) + sizeof(*req);
}

int decode_oem_meta_file_io_write_req(
    const struct pldm_msg* msg, size_t payload_length,
    struct pldm_oem_meta_file_io_write_req* req, size_t req_length)
{
    if (msg == nullptr || req == nullptr)
    {
        return -EINVAL;
    }

    if (req_length < sizeof(*req))
    {
        return -EINVAL;
    }

    if (payload_length < PLDM_OEM_META_FILE_IO_WRITE_REQ_MIN_LENGTH)
    {
        return -EOVERFLOW;
    }

    const auto* payload = msg->payload;
    req->handle = payload[0];

    uint32_t encodedLength{};
    memcpy(&encodedLength, payload + 1, sizeof(encodedLength));
    req->length = le32toh(encodedLength);

    if (req->length > (req_length - sizeof(*req)))
    {
        return -EOVERFLOW;
    }

    if (payload_length !=
        (PLDM_OEM_META_FILE_IO_WRITE_REQ_MIN_LENGTH + req->length))
    {
        return -EOVERFLOW;
    }

    memcpy(pldm_oem_meta_file_io_write_req_data(req),
           payload + PLDM_OEM_META_FILE_IO_WRITE_REQ_MIN_LENGTH, req->length);

    return 0;
}

int decode_oem_meta_file_io_read_req(
    const struct pldm_msg* msg, size_t payload_length,
    struct pldm_oem_meta_file_io_read_req* req)
{
    if (msg == nullptr || req == nullptr)
    {
        return -EINVAL;
    }

    if (req->version > sizeof(struct pldm_oem_meta_file_io_read_req))
    {
        return -E2BIG;
    }

    if (payload_length < PLDM_OEM_META_FILE_IO_READ_REQ_MIN_LENGTH)
    {
        return -EOVERFLOW;
    }

    const auto* payload = msg->payload;
    req->handle = payload[0];
    req->option = payload[1];
    req->length = payload[2];

    switch (req->option)
    {
        case PLDM_OEM_META_FILE_IO_READ_ATTR:
            if (req->length != 0 || payload_length !=
                                        PLDM_OEM_META_FILE_IO_READ_REQ_MIN_LENGTH)
            {
                return -EPROTO;
            }
            break;
        case PLDM_OEM_META_FILE_IO_READ_DATA:
        {
            if (req->length != PLDM_OEM_META_FILE_IO_READ_DATA_INFO_LENGTH)
            {
                return -EPROTO;
            }
            if (payload_length <
                (PLDM_OEM_META_FILE_IO_READ_REQ_MIN_LENGTH +
                 PLDM_OEM_META_FILE_IO_READ_DATA_INFO_LENGTH))
            {
                return -EOVERFLOW;
            }
            if (payload_length !=
                (PLDM_OEM_META_FILE_IO_READ_REQ_MIN_LENGTH +
                 PLDM_OEM_META_FILE_IO_READ_DATA_INFO_LENGTH))
            {
                return -EPROTO;
            }

            req->info.data.transferFlag = payload[3];
            uint16_t encodedOffset{};
            memcpy(&encodedOffset, payload + 4, sizeof(encodedOffset));
            req->info.data.offset = le16toh(encodedOffset);
            break;
        }
        default:
            return -EPROTO;
    }

    return 0;
}

void* pldm_oem_meta_file_io_read_resp_data(
    struct pldm_oem_meta_file_io_read_resp* resp)
{
    if (resp == nullptr)
    {
        return nullptr;
    }

    return reinterpret_cast<uint8_t*>(resp) + sizeof(*resp);
}

int encode_oem_meta_file_io_read_resp(
    uint8_t instance_id, struct pldm_oem_meta_file_io_read_resp* resp,
    size_t resp_len, struct pldm_msg* responseMsg, size_t payload_length)
{
    if (resp == nullptr || responseMsg == nullptr)
    {
        return -EINVAL;
    }

    if (resp_len < sizeof(*resp))
    {
        return -EINVAL;
    }

    if (resp->version > sizeof(*resp))
    {
        return -E2BIG;
    }

    size_t minPayloadLength = PLDM_OEM_META_FILE_IO_READ_RESP_MIN_SIZE;
    switch (resp->option)
    {
        case PLDM_OEM_META_FILE_IO_READ_ATTR:
            minPayloadLength += PLDM_OEM_META_FILE_IO_READ_ATTR_INFO_LENGTH;
            break;
        case PLDM_OEM_META_FILE_IO_READ_DATA:
            minPayloadLength += PLDM_OEM_META_FILE_IO_READ_DATA_INFO_LENGTH +
                                resp->length;
            if (resp_len < (sizeof(*resp) + resp->length))
            {
                return -EOVERFLOW;
            }
            break;
        default:
            return -EPROTO;
    }

    if (payload_length < minPayloadLength)
    {
        return -EOVERFLOW;
    }

    struct pldm_header_info header {};
    header.instance = instance_id;
    header.msg_type = PLDM_RESPONSE;
    header.pldm_type = PLDM_OEM;
    header.command = PLDM_OEM_META_FILE_IO_CMD_READ_FILE;
    auto rc = pack_pldm_header(&header, &(responseMsg->hdr));
    if (rc != PLDM_SUCCESS)
    {
        return rc;
    }

    auto* payload = responseMsg->payload;
    payload[0] = resp->completion_code;
    payload[1] = resp->handle;
    payload[2] = resp->option;
    payload[3] = resp->length;

    if (resp->option == PLDM_OEM_META_FILE_IO_READ_ATTR)
    {
        uint16_t size = htole16(resp->info.attr.size);
        uint32_t crc32 = htole32(resp->info.attr.crc32);
        memcpy(payload + 4, &size, sizeof(size));
        memcpy(payload + 6, &crc32, sizeof(crc32));
        return 0;
    }

    payload[4] = resp->info.data.transferFlag;
    uint16_t offset = htole16(resp->info.data.offset);
    memcpy(payload + 5, &offset, sizeof(offset));
    memcpy(payload + 7, pldm_oem_meta_file_io_read_resp_data(resp),
           resp->length);

    return 0;
}

} // extern "C"
