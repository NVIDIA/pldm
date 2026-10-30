# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""End-to-end package creation, inspection, extraction and rebuild tests."""

import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


TOOLS = Path(__file__).resolve().parents[1]
UNPACK = TOOLS / "fwpkg_unpack.py"
CREATOR = TOOLS / "pldm_fwup_pkg_creator.py"


class DownstreamCliTest(unittest.TestCase):
    """Exercise the public CLI with real package bytes and image payloads."""

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def run_tool(self, script, *args):
        result = subprocess.run(
            [sys.executable, str(script), *map(str, args)],
            cwd=self.work, capture_output=True, text=True, check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def inspect(self, package, option, *args):
        return json.loads(self.run_tool(UNPACK, package, option, *args))

    def make_package(self, metadata, name="test"):
        path = self.work / (name + ".json")
        path.write_text(json.dumps(metadata), encoding="utf-8")
        images = []
        for index, _ in enumerate(metadata["ComponentImageInformationArea"]):
            image = self.work / (name + str(index) + ".bin")
            image.write_bytes(bytes([index]) * (257 + index))
            images.append(image)
        package = self.work / (name + ".fwpkg")
        self.run_tool(CREATOR, package, path, *images)
        return package, images

    def assert_all_modes(self, metadata, name):
        package, images = self.make_package(metadata, name)
        content = self.inspect(package, "--show_pkg_content", "--verbose")
        unpacked = self.inspect(
            package, "--unpack", "--outdir", self.work / name, "--verbose")
        full = self.inspect(package, "--show_all_metadata")
        builder = self.inspect(package, "--dump_builder_json")
        expected_fd = metadata["FirmwareDeviceIdentificationArea"]
        expected_dd = metadata.get("DownstreamDeviceIdentificationArea", [])
        self.assertEqual(len(content["FirmwareDeviceRecords"]), len(expected_fd))
        self.assertEqual(len(content.get("DownstreamDeviceRecords", [])),
                         len(expected_dd))
        self.assertEqual(len(full["DownstreamDeviceIdentificationArea"].get(
            "DownstreamDeviceIDRecords", [])), len(expected_dd))
        self.assertEqual(len(builder.get("DownstreamDeviceIdentificationArea", [])),
                         len(expected_dd))
        ordered_images = {}
        for group, expected in (("FirmwareDeviceRecords", expected_fd),
                                ("DownstreamDeviceRecords", expected_dd)):
            for index, record in enumerate(expected):
                indices = record["ApplicableComponents"]
                shown = content[group][index]
                extracted = unpacked[group][index]
                self.assertEqual(len(shown["Components"]), len(indices))
                for component, output, image_index in zip(
                        shown["Components"], extracted["Components"], indices):
                    info = metadata["ComponentImageInformationArea"][image_index]
                    self.assertEqual(component["ComponentIdentifier"],
                                     hex(info["ComponentIdentifier"]))
                    self.assertEqual(component["ComponentVersionString"],
                                     info["ComponentVersionString"])
                    image = Path(output["FWImage"])
                    self.assertEqual(image.read_bytes(), images[image_index].read_bytes())
                    self.assertEqual(output["FWImageSHA256"],
                                     hashlib.sha256(image.read_bytes()).hexdigest())
                    ordered_images[image_index] = image
                if group == "DownstreamDeviceRecords":
                    self.assertEqual(shown["ApplicableComponents"], indices)
                    self.assertNotIn("ComponentImageSetVersionString", shown)
                    self.assertEqual(shown["DownstreamDevicePackageData"],
                                     record.get("DownstreamDevicePackageData", ""))
        if not expected_dd:
            self.assertNotIn("DownstreamDeviceRecords", content)
        builder_path = self.work / (name + "-builder.json")
        builder_path.write_text(json.dumps(builder), encoding="utf-8")
        rebuilt = self.work / (name + "-rebuilt.fwpkg")
        self.run_tool(CREATOR, rebuilt, builder_path,
                      *(ordered_images[i] for i in range(len(images))))
        self.assertEqual(self.inspect(rebuilt, "--dump_builder_json"), builder)
        self.assertEqual(rebuilt.read_bytes(), package.read_bytes())
        return content, full, builder

    def test_shipped_examples(self):
        for name in ("metadata-example-1.0.0",
                     "metadata-example-downstream-device",
                     "metadata-example-fdp-downstream"):
            with self.subTest(example=name):
                metadata = json.loads((TOOLS / (name + ".json")).read_text())
                self.assert_all_modes(metadata, name)

    def test_package_data_activation_and_manifest_roundtrip(self):
        identifiers = {2: "1244D2648D7D4718A030FC8A56587D5A",
                       3: "3119CE2FE80A4A99AF6D46F8B121F6BF",
                       4: "7B291C996DB64208801B02026E463C78"}
        for revision, identifier in identifiers.items():
            with self.subTest(revision=revision):
                metadata = json.loads((TOOLS / "metadata-example-downstream-device.json").read_text())
                metadata["PackageHeaderInformation"].update(
                    PackageHeaderFormatVersion=revision,
                    PackageHeaderIdentifier=identifier)
                record = metadata["DownstreamDeviceIdentificationArea"][0]
                record.update(
                    DownstreamDevicePackageData="00010280ff",
                    DownstreamDeviceUpdateOptionFlags=[0],
                    DownstreamDeviceSelfContainedActivationMinVersionString="1.2",
                    DownstreamDeviceSelfContainedActivationMinVersionComparisonStamp=123)
                # Also cover the downstream-only SCSI initial descriptor.
                record["Descriptors"][0] = {
                    "DescriptorType": 6, "DescriptorData": "56454e444f522020"}
                if revision == 4:
                    record["DownstreamDeviceReferenceManifestData"] = "102000ff"
                _, full, builder = self.assert_all_modes(metadata, str(revision))
                self.assertEqual(full["DownstreamDeviceIdentificationArea"][
                    "DownstreamDeviceIDRecords"][0]["DownstreamDevicePackageDataLength"], 5)
                self.assertEqual(builder["DownstreamDeviceIdentificationArea"][0][
                    "DownstreamDevicePackageData"], "00010280ff")
                if revision == 4:
                    self.assertEqual(builder["DownstreamDeviceIdentificationArea"][0][
                        "DownstreamDeviceReferenceManifestData"], "102000ff")

    def test_shared_components_and_bitmap_across_bytes(self):
        metadata = json.loads((TOOLS / "metadata-example-downstream-device.json").read_text())
        image = metadata["ComponentImageInformationArea"][0]
        metadata["ComponentImageInformationArea"] = [
            dict(image, ComponentIdentifier=i, ComponentVersionString=str(i))
            for i in range(9)]
        record = metadata["DownstreamDeviceIdentificationArea"][0]
        record["ApplicableComponents"] = list(range(9))
        second = copy.deepcopy(record)
        second["ApplicableComponents"] = [0, 8]
        metadata["DownstreamDeviceIdentificationArea"].append(second)
        self.assert_all_modes(metadata, "shared")

    def test_invalid_package_data_is_rejected(self):
        for data in ("xyz", "f", 123, "00" * 65536):
            with self.subTest(data_type=type(data).__name__, length=len(str(data))):
                metadata = json.loads((TOOLS / "metadata-example-downstream-device.json").read_text())
                metadata["DownstreamDeviceIdentificationArea"][0]["DownstreamDevicePackageData"] = data
                path = self.work / "invalid.json"
                path.write_text(json.dumps(metadata), encoding="utf-8")
                image = self.work / "image.bin"
                image.write_bytes(b"payload")
                result = subprocess.run(
                    [sys.executable, str(CREATOR), str(self.work / "invalid.fwpkg"),
                     str(path), str(image)], capture_output=True, text=True, check=False)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("ERROR:", result.stderr)
                self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
