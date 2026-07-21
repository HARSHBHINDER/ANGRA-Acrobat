cmake_minimum_required(VERSION 3.28)
project(AngraAcrobat VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets PrintSupport)

# PDFium: prebuilt binary drop, root given by PDFIUM_DIR (cache var or env).
set(PDFIUM_DIR "$ENV{PDFIUM_DIR}" CACHE PATH "PDFium root containing include/ and lib/")
find_path(PDFIUM_INCLUDE_DIR fpdfview.h HINTS "${PDFIUM_DIR}/include")
find_library(PDFIUM_LIBRARY NAMES pdfium pdfium.dll HINTS "${PDFIUM_DIR}/lib")
if(NOT PDFIUM_INCLUDE_DIR OR NOT PDFIUM_LIBRARY)
    message(FATAL_ERROR "PDFium not found. Set PDFIUM_DIR to a prebuilt PDFium "
                        "(include/ and lib/). See docs/DEPENDENCIES.md.")
endif()

add_library(pdfcore STATIC src/PdfDocument.cpp)
target_include_directories(pdfcore PUBLIC src "${PDFIUM_INCLUDE_DIR}")
target_link_libraries(pdfcore PUBLIC Qt6::Gui "${PDFIUM_LIBRARY}")

add_executable(ANGRA WIN32 src/main.cpp)
target_link_libraries(ANGRA PRIVATE pdfcore Qt6::Widgets Qt6::PrintSupport)
set_target_properties(ANGRA PROPERTIES OUTPUT_NAME "ANGRA")

if(EXISTS "${PDFIUM_DIR}/bin/pdfium.dll")
    add_custom_command(TARGET ANGRA POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${PDFIUM_DIR}/bin/pdfium.dll" "$<TARGET_FILE_DIR:ANGRA>")
endif()

enable_testing()
add_executable(test_pdfdocument tests/test_pdfdocument.cpp)
target_link_libraries(test_pdfdocument PRIVATE pdfcore)
add_test(NAME pdfdocument
         COMMAND test_pdfdocument "${CMAKE_SOURCE_DIR}/resources/test.pdf")

# qpdf (optional): enables encryption/decryption/sanitize/optimize/repair.
set(QPDF_DIR "$ENV{QPDF_DIR}" CACHE PATH "qpdf root containing include/ and lib/")
find_path(QPDF_INCLUDE_DIR qpdf/QPDF.hh HINTS "${QPDF_DIR}/include")
find_library(QPDF_LIBRARY NAMES qpdf qpdf29 qpdf30 HINTS "${QPDF_DIR}/lib")
if(QPDF_INCLUDE_DIR AND QPDF_LIBRARY)
    target_sources(pdfcore PRIVATE src/PdfProtect.cpp)
    target_include_directories(pdfcore PUBLIC "${QPDF_INCLUDE_DIR}")
    target_link_libraries(pdfcore PUBLIC "${QPDF_LIBRARY}")
    target_compile_definitions(pdfcore PUBLIC ANGRA_HAVE_QPDF)
    file(GLOB QPDF_DLLS "${QPDF_DIR}/bin/*.dll")
    foreach(dll ${QPDF_DLLS})
        add_custom_command(TARGET ANGRA POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:ANGRA>")
    endforeach()
else()
    message(STATUS "qpdf not found - protection features disabled (set QPDF_DIR)")
endif()
