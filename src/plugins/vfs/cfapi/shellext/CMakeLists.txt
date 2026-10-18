# SPDX-FileCopyrightText: 2022 Nextcloud GmbH and Nextcloud contributors
# SPDX-FileCopyrightText: 2026 OpenCloud contributors
# SPDX-License-Identifier: GPL-2.0-or-later

# Windows Explorer shell extension for the cfapi vfs plugin.
# Only the thumbnail handler is built: Explorer loads it (in a dllhost surrogate) to get previews
# of dehydrated placeholders, it asks the running client for them over a local socket.
# The custom state (overlay icon) provider in this folder is not built.

unset(CMAKE_CXX_CLANG_TIDY)

set(CFAPI_SHELL_EXTENSIONS_LIB_NAME "${APPLICATION_EXECUTABLE}_cfapi_shellext" PARENT_SCOPE)
set(CFAPI_SHELL_EXTENSIONS_LIB_NAME "${APPLICATION_EXECUTABLE}_cfapi_shellext")

# Name based (stable) ids: a rebranded or beta build gets its own COM registration.
set(_cfapi_shellext_namespace "77f11bde-8120-4ee3-b037-99c20cf03735")
string(UUID CFAPI_SHELLEXT_THUMBNAIL_HANDLER_CLASS_ID NAMESPACE ${_cfapi_shellext_namespace} NAME "${APPLICATION_SHORTNAME}:ThumbnailProvider" TYPE SHA1 UPPER)
string(UUID CFAPI_SHELLEXT_APPID NAMESPACE ${_cfapi_shellext_namespace} NAME "${APPLICATION_SHORTNAME}:ShellExtensionsAppId" TYPE SHA1 UPPER)
message(STATUS "cfapi thumbnail handler CLSID: {${CFAPI_SHELLEXT_THUMBNAIL_HANDLER_CLASS_ID}}")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/configvfscfapishellext.h.in ${CMAKE_CURRENT_BINARY_DIR}/configvfscfapishellext.h)

add_library(CfApiShellExtensions MODULE
    dllmain.cpp
    cfapishellintegrationclassfactory.cpp
    thumbnailprovider.cpp
    thumbnailprovideripc.cpp
    CfApiShellIntegration.def
)
apply_common_target_settings(CfApiShellExtensions)

target_link_libraries(CfApiShellExtensions PRIVATE shlwapi gdiplus ole32 Qt::Core Qt::Network)
target_include_directories(CfApiShellExtensions PRIVATE ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_SOURCE_DIR}/src)

set_target_properties(CfApiShellExtensions PROPERTIES OUTPUT_NAME ${CFAPI_SHELL_EXTENSIONS_LIB_NAME})
if (CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    # the vfs plugin registers the DLL from the folder of the client executable
    set_target_properties(CfApiShellExtensions PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()

# a MODULE is a LIBRARY target, also on Windows: install it next to the executable
install(TARGETS CfApiShellExtensions LIBRARY DESTINATION ${KDE_INSTALL_BINDIR})
