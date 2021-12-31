project(linux-pipewire)

option(ENABLE_PIPEWIRE "Enable PipeWire support" ON)
if(NOT ENABLE_PIPEWIRE)
	message(STATUS "PipeWire support disabled, linux-pipewire plugin disabled")
	return()
endif()

find_package(PipeWire QUIET)
find_package(Libdrm QUIET) # we require libdrm/drm_fourcc.h to build
find_package(Gio QUIET)

if(NOT PIPEWIRE_FOUND)
	message(FATAL_ERROR "PipeWire library not found! Please install PipeWire or set ENABLE_PIPEWIRE=OFF")
elseif(NOT GIO_FOUND)
	message(FATAL_ERROR "Gio library not found! Please install GLib2 (or Gio) or set ENABLE_PIPEWIRE=OFF")
elseif(NOT LIBDRM_INCLUDE_DIRS)
	message(FATAL_ERROR "libdrm headers not found! Please install libdrm or set ENABLE_PIPEWIRE=OFF")
endif()

add_definitions(-DENABLE_PIPEWIRE)

set(linux-pipewire_INCLUDES
	"${CMAKE_SOURCE_DIR}/libobs"
	${GIO_INCLUDE_DIRS}
	${PIPEWIRE_INCLUDE_DIRS}
)

add_definitions(
	${GIO_DEFINITIONS}
	${PIPEWIRE_DEFINITIONS}
)

set(linux-pipewire_SOURCES
	linux-pipewire.c
	pipewire-common.c
	pipewire-virtualcam.c
	pipewire.c
	pipewire-capture.c
	portal.c
)
set(linux-pipewire_HEADERS
	pipewire-common.h
	pipewire-virtualcam.h
	pipewire.h
	pipewire-capture.h
	portal.h
)

set(linux-pipewire_LIBRARIES
	libobs
	glad
	${GIO_LIBRARIES}
	${PIPEWIRE_LIBRARIES}
)

include_directories(SYSTEM
	${linux-pipewire_INCLUDES}
)
add_library(linux-pipewire MODULE
	${linux-pipewire_SOURCES}
	${linux-pipewire_HEADERS}
)
target_link_libraries(linux-pipewire
	${linux-pipewire_LIBRARIES}
)

set_target_properties(linux-pipewire PROPERTIES FOLDER "plugins")

install_obs_plugin_with_data(linux-pipewire data)
