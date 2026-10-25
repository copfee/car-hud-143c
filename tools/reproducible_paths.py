"""Keep machine-specific build paths out of firmware diagnostic strings."""
Import("env")  # PlatformIO / SCons injects the active construction environment.

env.Append(CCFLAGS=[
    "-ffile-prefix-map=" + env.subst("$PROJECT_DIR") + "=.",
    "-ffile-prefix-map=" + env.subst("$PROJECT_PACKAGES_DIR") + "=/platformio/packages",
])
