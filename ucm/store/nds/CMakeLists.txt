file(GLOB_RECURSE UCM_NDS_CORE_CC_SOURCE_FILES "./cc/*.cc")
list(FILTER UCM_NDS_CORE_CC_SOURCE_FILES EXCLUDE REGEX "nds_client\\.cc$")
add_library(ndscore OBJECT ${UCM_NDS_CORE_CC_SOURCE_FILES})
set_target_properties(ndscore PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ndscore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/cc)
target_link_libraries(ndscore PUBLIC storeintf infra_logger)

if(BUILD_UCM_NDS)
    find_path(NDS_INTERFACE_INCLUDE_DIR
        NAMES nds_interface.h
        HINTS "${NDS_INCLUDE_DIR}" "${NDS_ROOT}"
        PATH_SUFFIXES vllm/NDS_include include
    )
    find_library(NDSKV_LIBRARY
        NAMES ndskv
        HINTS "${NDS_LIBRARY_DIR}" "${NDS_ROOT}"
        PATH_SUFFIXES vllm/NDS_bin vllm/NDS3.0/NDS_bin vllm/NDS3.0 _pynds_kv_native lib lib64
    )
    if(NOT NDS_INTERFACE_INCLUDE_DIR)
        message(FATAL_ERROR
            "BUILD_UCM_NDS=ON requires nds_interface.h; set NDS_ROOT or NDS_INCLUDE_DIR")
    endif()
    if(NOT NDSKV_LIBRARY)
        message(FATAL_ERROR
            "BUILD_UCM_NDS=ON requires libndskv.so; set NDS_ROOT or NDS_LIBRARY_DIR")
    endif()

    add_library(ndsstore SHARED ./cc/nds_client.cc)
    target_include_directories(ndsstore PRIVATE ${NDS_INTERFACE_INCLUDE_DIR})
    target_link_libraries(ndsstore PUBLIC ndscore PRIVATE ${NDSKV_LIBRARY} ${CMAKE_DL_LIBS})
    # Deploy libndskv.so beside libndsstore.so, or expose its existing installation
    # through LD_LIBRARY_PATH. No machine-specific NDS path is embedded here.
    set_target_properties(ndsstore PROPERTIES INSTALL_RPATH "$ORIGIN")
    file(RELATIVE_PATH INSTALL_REL_PATH ${UCM_ROOT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
    install(TARGETS ndsstore LIBRARY DESTINATION ${INSTALL_REL_PATH} COMPONENT ucm)
endif()
