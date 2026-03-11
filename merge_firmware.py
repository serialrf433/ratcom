"""Post-build script: merge bootloader + partitions + boot_app0 + firmware
into a single .bin for M5Burner and one-step flashing."""

Import("env")

import os


def merge_bin(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")

    # boot_app0.bin lives in the Arduino framework tools
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    boot_app0 = os.path.join(framework_dir, "tools", "partitions", "boot_app0.bin")

    output = os.path.join(project_dir, "ratcom-merged.bin")

    env.Execute(
        "esptool.py --chip esp32s3 merge_bin "
        "--flash_mode dio --flash_size 8MB "
        f"-o {output} "
        f"0x0000 {build_dir}/bootloader.bin "
        f"0x8000 {build_dir}/partitions.bin "
        f"0xe000 {boot_app0} "
        f"0x10000 {build_dir}/firmware.bin"
    )
    print(f"\n** Merged firmware written to: {output}")


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_bin)
