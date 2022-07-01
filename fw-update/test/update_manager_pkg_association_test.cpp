#include "libpldm/firmware_update.h"

#include "common/utils.hpp"
#include "fw-update/update_manager.hpp"
#include "pldmd/dbus_impl_requester.hpp"
#include "requester/handler.hpp"
#include "requester/test/mock_request.hpp"

#include <sdeventplus/test/sdevent.hpp>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pldm::fw_update;
using namespace std::chrono;

class PackageAssociationEmptyTargetFiltering : public testing::Test
{
  protected:
    PackageAssociationEmptyTargetFiltering() :
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(fd, event, dbusImplRequester, false, 90000, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, dbusImplRequester, descriptorMap,
                      componentInfoMap, componentNameMap, false)
    {}

    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    int fd = -1;
    requester::Handler<requester::Request> reqHandler;
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;

    // Package to firmware device associations, the FD identifer records via
    // QueryIdentifiers command match to Package device identifer records.
    // No target filtering.

    // Device1 - ApplicableComponents{compIdentifer1, compIdentifer2}
    // Device2 - ApplicableComponents{compIdentifer1, compIdentifer3}
    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };

    const CompIdentifier compIdentifer1 = 65280;
    const CompIdentifier compIdentifer2 = 80;
    const CompIdentifier compIdentifer3 = 16;

    // Component Idenitifier field is relevant for the tests
    const ComponentImageInfos compImageInfos{
        {10, compIdentifer1, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString3"},
        {10, compIdentifer2, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString4"},
        {10, compIdentifer3, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString5"}};

    std::vector<sdbusplus::message::object_path> targets;
};

TEST_F(PackageAssociationEmptyTargetFiltering, MatchingDescriptors)
{
    constexpr EID eid1 = 13;
    constexpr EID eid2 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, componentNameMap,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };
    // All the components match for all the devices
    constexpr TotalComponentUpdates expectTotalComponents = 4;

    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
}

TEST_F(PackageAssociationEmptyTargetFiltering,
       MatchingDescriptorsMultipleDevices)
{
    constexpr EID eid1 = 13;
    constexpr EID eid2 = 14;
    constexpr EID eid3 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid3,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, componentNameMap,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{
        {eid2, 0}, {eid1, 1}, {eid3, 2}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };
    // 3 device * 2 components
    constexpr TotalComponentUpdates expectTotalComponents = 6;

    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
}

class PackageAssociationTargetFiltering : public testing::Test
{
  protected:
    PackageAssociationTargetFiltering() :
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(fd, event, dbusImplRequester, false, 90000, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, dbusImplRequester, descriptorMap,
                      componentInfoMap, componentNameMap, false)
    {}

    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    int fd = -1;
    requester::Handler<requester::Request> reqHandler;
    const ComponentInfoMap componentInfoMap;
    UpdateManager updateManager;

    // Device1 - ApplicableComponents{compIdentifer1, compIdentifer2}
    // Device2 - ApplicableComponents{compIdentifer1, compIdentifer3}
    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}},
         {}},
    };

    // Discovered two endpoints that match with the Device 1 & Device2
    // descriptors.
    const EID eid1 = 1;
    const EID eid2 = 2;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}}},
        {eid2,
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}}}};

    const CompIdentifier compIdentifer1 = 65280;
    const CompIdentifier compIdentifer2 = 80;
    const CompIdentifier compIdentifer3 = 16;

    // ComponentImageInformationArea from the package, what matter for the
    // test is the component identifers and order of the components.
    const ComponentImageInfos compImageInfos{
        {10, compIdentifer1, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString3"},
        {10, compIdentifer2, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString4"},
        {10, compIdentifer3, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString5"},
    };

    // ComponentNameMap is needed for target filtering feature and maps the
    // firmware targets to the right PLDM device and components.
    ComponentNameMap componentNameMap{
        {eid1, {{65280, "ERoT_FPGA_Firmware"}, {80, "FPGAFirmware"}}},
        {eid2, {{65280, "ERoT_HMC_Firmware"}, {16, "HMCFirmware"}}}};
};

TEST_F(PackageAssociationTargetFiltering, MatchingTwoComponents)
{

    const std::string erotFPGAFirmware =
        "/xyz/openbmc_project/software/ERoT_FPGA_Firmware";
    const std::string erotHMCFirmware =
        "/xyz/openbmc_project/software/ERoT_HMC_Firmware";
    std::vector<sdbusplus::message::object_path> targets{erotFPGAFirmware,
                                                         erotHMCFirmware};
    FirmwareDeviceIDRecords outFwDeviceIDRecords{};
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, componentNameMap,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0},
         "VersionString1",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}}},
         {}},
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}},
         {}},
    };
    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    constexpr TotalComponentUpdates expectTotalComponents = 2;

    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
}

TEST_F(PackageAssociationTargetFiltering, MatchingOneComponent)
{
    const std::string erotHMCFirmware =
        "/xyz/openbmc_project/software/ERoT_HMC_Firmware";
    std::vector<sdbusplus::message::object_path> targets{erotHMCFirmware};
    FirmwareDeviceIDRecords outFwDeviceIDRecords{};
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, componentNameMap,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0},
         "VersionString2",
         {{PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x01}}},
         {}},
    };
    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid2, 0}};
    constexpr TotalComponentUpdates expectTotalComponents = 1;

    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
}

class PackageAssociationMultipleDescSameType : public testing::Test
{
  protected:
    PackageAssociationMultipleDescSameType() :
        event(sdeventplus::Event::get_default()),
        dbusImplRequester(pldm::utils::DBusHandler::getBus(),
                          "/xyz/openbmc_project/pldm"),
        reqHandler(fd, event, dbusImplRequester, false, 90000, seconds(1), 2,
                   milliseconds(100)),
        updateManager(event, reqHandler, dbusImplRequester, descriptorMap,
                      componentInfoMap, componentNameMap, false)
    {}

    sdeventplus::Event event;
    pldm::dbus_api::Requester dbusImplRequester;
    int fd = -1;
    requester::Handler<requester::Request> reqHandler;
    const DescriptorMap descriptorMap;
    const ComponentInfoMap componentInfoMap;
    ComponentNameMap componentNameMap;
    UpdateManager updateManager;

    const FirmwareDeviceIDRecords inFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}}};

    const CompIdentifier compIdentifer1 = 65280;
    const CompIdentifier compIdentifer2 = 80;
    const CompIdentifier compIdentifer3 = 16;

    // Component Idenitifier field is relevant for the tests
    const ComponentImageInfos compImageInfos{
        {10, compIdentifer1, 0xFFFFFFFF, 0, 0, 326, 27, "VersionString3"},
        {10, compIdentifer2, 0xFFFFFFFF, 0, 1, 353, 27, "VersionString4"},
        {10, compIdentifer3, 0xFFFFFFFF, 1, 12, 380, 27, "VersionString5"}};

    std::vector<sdbusplus::message::object_path> targets;
};

TEST_F(PackageAssociationMultipleDescSameType, MultipleDescriptorsMatch)
{
    constexpr EID eid1 = 13;
    constexpr EID eid2 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, componentNameMap,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}},
    };
    // All the components match for all the devices
    constexpr TotalComponentUpdates expectTotalComponents = 4;

    EXPECT_EQ(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_EQ(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_EQ(totalNumComponentUpdates, expectTotalComponents);
}

TEST_F(PackageAssociationMultipleDescSameType, MultipleDescriptorsNoMatch)
{
    constexpr EID eid1 = 13;
    constexpr EID eid2 = 24;
    const DescriptorMap descriptorMap{
        {eid1,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x51})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU",
                           std::vector<uint8_t>{0x50, 0x51, 0x52, 0x53})}}},
        {eid2,
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,

           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}}}};

    FirmwareDeviceIDRecords outFwDeviceIDRecords;
    TotalComponentUpdates totalNumComponentUpdates = 0;

    auto deviceUpdaterInfos = updateManager.associatePkgToDevices(
        inFwDeviceIDRecords, descriptorMap, compImageInfos, componentNameMap,
        targets, outFwDeviceIDRecords, totalNumComponentUpdates);

    DeviceUpdaterInfos expectDeviceUpdaterInfos{{eid1, 0}, {eid2, 1}};
    const FirmwareDeviceIDRecords expectFwDeviceIDRecords{
        {1,
         {0, 1},
         "VersionString1",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x10, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x50})},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("SKU",
                           std::vector<uint8_t>{0x50, 0x51, 0x52, 0x53})}},
         {}},
        {1,
         {0, 2},
         "VersionString2",
         {{PLDM_FWUP_UUID,
           std::vector<uint8_t>{0x16, 0x20, 0x23, 0xC9, 0x3E, 0xC5, 0x41, 0x15,
                                0x95, 0xF4, 0x48, 0x70, 0x1D, 0x49, 0xD6,
                                0x75}},
          {PLDM_FWUP_IANA_ENTERPRISE_ID,
           std::vector<uint8_t>{0x47, 0x16, 0x00, 0x00}},
          {PLDM_FWUP_VENDOR_DEFINED,
           std::make_tuple("GLACIERDSD", std::vector<uint8_t>{0x10})}},
         {}}};
    // All the components match for all the devices
    constexpr TotalComponentUpdates expectTotalComponents = 4;

    EXPECT_NE(deviceUpdaterInfos, expectDeviceUpdaterInfos);
    EXPECT_NE(outFwDeviceIDRecords, expectFwDeviceIDRecords);
    EXPECT_NE(totalNumComponentUpdates, expectTotalComponents);
}
