if (WIN32)
    add_subdirectory(shellext)

    add_vfs_plugin(NAME cfapi
        SRC
            cfapiwrapper.cpp
            hydrationdevice.cpp
            shellextensionregistration.cpp
            vfs_cfapi.cpp
        LIBS
            cldapi
            WindowsApp
    )
    # configvfscfapishellext.h
    target_include_directories(vfs_cfapi PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shellext)
    # the client registers the DLL, make sure it is built with it
    add_dependencies(vfs_cfapi CfApiShellExtensions)
endif()
