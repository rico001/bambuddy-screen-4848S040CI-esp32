import os
Import("env")

# Remove ARM-specific assembly files (Helium, NEON) incompatible with Xtensa (ESP32)
def remove_arm_asm(*args, **kwargs):
    libdeps_dir = os.path.join(env.subst("$PROJECT_LIBDEPS_DIR"), env.subst("$PIOENV"))
    arm_dirs = ["helium", "neon"]
    for root, dirs, files in os.walk(libdeps_dir):
        for f in files:
            if f.endswith(".S") and any(d in root for d in arm_dirs):
                path = os.path.join(root, f)
                if os.path.exists(path):
                    os.remove(path)
                    print(f"Removed incompatible ARM assembly file: {path}")

remove_arm_asm()
