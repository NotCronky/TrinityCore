# Included by src/server/scripts/CMakeLists.txt after the module's target exists (see modules/README.md).
#
# The profile loader reads YAML with fkYAML, a header-only library bundled in deps/fkYAML (MIT). The
# include path and the define are set on that one source file, not on the target, so adding them
# doesn't rebuild every other script when the module is built statically into the scripts target.
#
# ROTATION_BOT_SOURCE_PROFILE_DIR makes the profiles folder in this module's source tree the
# default for RotationBot.ProfileDir, so profiles edited there apply with ".rot reload".
set_property(SOURCE "${MODULE_ROOT}/src/RotationProfiles.cpp"
    APPEND PROPERTY INCLUDE_DIRECTORIES "${MODULE_ROOT}/deps/fkYAML")

set_property(SOURCE "${MODULE_ROOT}/src/RotationProfiles.cpp"
    APPEND PROPERTY COMPILE_DEFINITIONS "ROTATION_BOT_SOURCE_PROFILE_DIR=\"${MODULE_ROOT}/profiles\"")
