option( PLUGIN_STANDARD_LMI_TIFFBMP "Install LmiTiffBmpPlugin" OFF )

if( PLUGIN_STANDARD_LMI_TIFFBMP )
    project( LMI_TIFFBMP_PLUGIN )

    AddPlugin( NAME ${PROJECT_NAME} )

    add_subdirectory( include )
    add_subdirectory( src )

    # QCC_DB_LIB 提供 ccPointCloud / ccMesh 等数据库类
    # QCC_GL_LIB 提供 ccGLWindowInterface（视角保存/恢复）
    target_link_libraries( ${PROJECT_NAME}
        QCC_DB_LIB
        QCC_GL_LIB
        Qt6::Widgets
        Qt6::Concurrent
    )

endif()
