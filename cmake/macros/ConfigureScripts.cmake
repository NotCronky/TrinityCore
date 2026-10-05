# This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
#
# This file is free software; as a special exception the author gives
# unlimited permission to copy and/or distribute it, with or without
# modifications, as long as this notice is preserved.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY, to the extent permitted by law; without even the
# implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

# Returns the base path to the script directory in the source directory
function(WarnAboutSpacesInBuildPath)
  # Only check win32 since unix doesn't allow spaces in paths
  if(WIN32)
    string(FIND "${CMAKE_BINARY_DIR}" " " SPACE_INDEX_POS)

    if(SPACE_INDEX_POS GREATER -1)
      message("")
      message(WARNING " *** WARNING!\n"
                      " *** Your selected build directory contains spaces!\n"
                      " *** Please note that this will cause issues!")
    endif()
  endif()
endfunction()

# Returns the base path to the script directory in the source directory
function(GetScriptsBasePath variable)
  set(${variable} "${CMAKE_SOURCE_DIR}/src/server/scripts" PARENT_SCOPE)
endfunction()

# Returns the base path to the modules directory in the source directory
function(GetModulesBasePath variable)
  set(${variable} "${CMAKE_SOURCE_DIR}/modules" PARENT_SCOPE)
endfunction()

# Stores the root directory of the given module in the variable
# when it is a module in modules/, or an empty string otherwise
function(GetModuleRootOfScriptModule module variable)
  GetScriptsBasePath(SCRIPTS_BASE_PATH)
  GetModulesBasePath(MODULES_BASE_PATH)
  if(NOT IS_DIRECTORY "${SCRIPTS_BASE_PATH}/${module}" AND IS_DIRECTORY "${MODULES_BASE_PATH}/${module}/src")
    set(${variable} "${MODULES_BASE_PATH}/${module}" PARENT_SCOPE)
  else()
    set(${variable} "" PARENT_SCOPE)
  endif()
endfunction()

# Stores the absolut path of the given module in the variable
function(GetPathToScriptModule module variable)
  GetModuleRootOfScriptModule(${module} MODULE_ROOT)
  if(MODULE_ROOT)
    set(${variable} "${MODULE_ROOT}/src" PARENT_SCOPE)
  else()
    GetScriptsBasePath(SCRIPTS_BASE_PATH)
    set(${variable} "${SCRIPTS_BASE_PATH}/${module}" PARENT_SCOPE)
  endif()
endfunction()

# Stores the project name of the given module in the variable
function(GetProjectNameOfScriptModule module variable)
  string(TOLOWER "scripts_${SCRIPT_MODULE}" GENERATED_NAME)
  set(${variable} "${GENERATED_NAME}" PARENT_SCOPE)
endfunction()

# Creates a list of all script modules
# and stores it in the given variable.
function(GetScriptModuleList variable)
  GetScriptsBasePath(BASE_PATH)
  file(GLOB LOCALE_SCRIPT_MODULE_LIST RELATIVE
    ${BASE_PATH}
    ${BASE_PATH}/*)

  set(${variable})
  foreach(SCRIPT_MODULE ${LOCALE_SCRIPT_MODULE_LIST})
    GetPathToScriptModule(${SCRIPT_MODULE} SCRIPT_MODULE_PATH)
    if(IS_DIRECTORY ${SCRIPT_MODULE_PATH})
      list(APPEND ${variable} ${SCRIPT_MODULE})
    endif()
  endforeach()

  # Modules in modules/<name>/ with their sources in modules/<name>/src
  GetModulesBasePath(MODULES_BASE_PATH)
  file(GLOB LOCALE_MODULE_LIST RELATIVE
    ${MODULES_BASE_PATH}
    ${MODULES_BASE_PATH}/*)
  foreach(MODULE ${LOCALE_MODULE_LIST})
    if(IS_DIRECTORY "${MODULES_BASE_PATH}/${MODULE}/src")
      if(MODULE IN_LIST ${variable})
        message(FATAL_ERROR "Module \"${MODULE}\" has the same name as the script directory src/server/scripts/${MODULE}, rename the module.")
      endif()
      list(APPEND ${variable} ${MODULE})
    endif()
  endforeach()
  set(${variable} ${${variable}} PARENT_SCOPE)
endfunction()

# Converts the given script module name into it's
# variable name which holds the linkage type.
function(ScriptModuleNameToVariable module variable)
  string(MAKE_C_IDENTIFIER ${module} ${variable})
  string(TOUPPER ${${variable}} ${variable})
  set(${variable} "SCRIPTS_${${variable}}")
  set(${variable} ${${variable}} PARENT_SCOPE)
endfunction()

# Stores in the given variable whether dynamic linking is required
function(IsDynamicLinkingRequired variable)
  if(SCRIPTS MATCHES "dynamic")
    set(IS_DEFAULT_VALUE_DYNAMIC ON)
  endif()

  GetScriptModuleList(SCRIPT_MODULE_LIST)
  set(IS_REQUIRED OFF)
  foreach(SCRIPT_MODULE ${SCRIPT_MODULE_LIST})
    ScriptModuleNameToVariable(${SCRIPT_MODULE} SCRIPT_MODULE_VARIABLE)
    if((${SCRIPT_MODULE_VARIABLE} STREQUAL "dynamic") OR
        (${SCRIPT_MODULE_VARIABLE} STREQUAL "default" AND IS_DEFAULT_VALUE_DYNAMIC))
      set(IS_REQUIRED ON)
      break()
    endif()
  endforeach()
  set(${variable} ${IS_REQUIRED} PARENT_SCOPE)
endfunction()

# Stores the native variable name
function(GetNativeSharedLibraryName module variable)
  if(WIN32)
    set(${variable} "${module}.dll" PARENT_SCOPE)
  elseif(APPLE)
    set(${variable} "lib${module}.dylib" PARENT_SCOPE)
  else()
    set(${variable} "lib${module}.so" PARENT_SCOPE)
  endif()
endfunction()

# Stores the native install path in the variable
function(GetInstallOffset variable)
  if(WIN32)
    set(${variable} "${CMAKE_INSTALL_PREFIX}/scripts" PARENT_SCOPE)
  else()
    set(${variable} "${CMAKE_INSTALL_PREFIX}/bin/scripts" PARENT_SCOPE)
  endif()
endfunction()

# Installs the config files of a module (modules/<name>/conf/*.conf.dist) into
# worldserver.conf.d, which the worldserver loads after worldserver.conf.
# The .conf.dist is replaced on every install; the .conf is only created
# when it doesn't exist yet, so changed settings are kept.
function(InstallModuleConfigs module_root)
  file(GLOB MODULE_CONFIGS "${module_root}/conf/*.conf.dist")
  if(NOT COPY_CONF OR NOT MODULE_CONFIGS)
    return()
  endif()

  if(WIN32)
    set(MODULE_CONF_DIR "${CMAKE_INSTALL_PREFIX}/worldserver.conf.d")
  else()
    set(MODULE_CONF_DIR "${CONF_DIR}/worldserver.conf.d")
  endif()

  install(FILES ${MODULE_CONFIGS} DESTINATION "${MODULE_CONF_DIR}")
  foreach(MODULE_CONFIG ${MODULE_CONFIGS})
    # The config loader only reads the settings under the file's first section
    file(STRINGS "${MODULE_CONFIG}" MODULE_CONFIG_SECTION REGEX "^\\[worldserver\\]")
    if(NOT MODULE_CONFIG_SECTION)
      message(WARNING "${MODULE_CONFIG} has no [worldserver] section header, so the worldserver will ignore its settings.")
    endif()

    get_filename_component(MODULE_CONFIG_NAME "${MODULE_CONFIG}" NAME)
    string(REGEX REPLACE "\\.dist$" "" MODULE_CONFIG_NAME "${MODULE_CONFIG_NAME}")
    install(CODE "
      if(NOT EXISTS \"\$ENV{DESTDIR}${MODULE_CONF_DIR}/${MODULE_CONFIG_NAME}\")
        message(STATUS \"Creating: \$ENV{DESTDIR}${MODULE_CONF_DIR}/${MODULE_CONFIG_NAME}\")
        configure_file(\"${MODULE_CONFIG}\" \"\$ENV{DESTDIR}${MODULE_CONF_DIR}/${MODULE_CONFIG_NAME}\" COPYONLY)
      endif()
    ")
  endforeach()
endfunction()

# Includes modules/<name>/module.cmake when the module has one, so the module
# can add libraries, include directories or definitions to the target its
# sources are built into. Available in module.cmake:
#   MODULE_NAME   the module's directory name, e.g. mod-hello
#   MODULE_ROOT   the module's root directory
#   MODULE_TARGET the target the module is built into ("scripts" when static)
function(IncludeModuleCMake module module_root target)
  if(EXISTS "${module_root}/module.cmake")
    set(MODULE_NAME ${module})
    set(MODULE_ROOT ${module_root})
    set(MODULE_TARGET ${target})
    include("${module_root}/module.cmake")
  endif()
endfunction()
