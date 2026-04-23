target_sources( ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/LmiTiffBmpPlugin.h
        ${CMAKE_CURRENT_LIST_DIR}/TiffBmpLoader.h
        ${CMAKE_CURRENT_LIST_DIR}/TiffBmpPanel.h
        ${CMAKE_CURRENT_LIST_DIR}/FlexibleDoubleSpinBox.h
        ${CMAKE_CURRENT_LIST_DIR}/DisplayModeDialog.h
)

target_include_directories( ${PROJECT_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
