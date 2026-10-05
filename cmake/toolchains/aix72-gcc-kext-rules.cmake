# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

foreach(_aix_cross_link_flags IN ITEMS CMAKE_C_LINK_FLAGS CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS CMAKE_SHARED_LIBRARY_LINK_C_FLAGS)
    if(DEFINED ${_aix_cross_link_flags})
        string(REPLACE "-Wl,-bnoipath" "" ${_aix_cross_link_flags} "${${_aix_cross_link_flags}}")
    endif()
endforeach()

unset(_aix_cross_link_flags)
