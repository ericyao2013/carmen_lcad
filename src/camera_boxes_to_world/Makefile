include ../Makefile.conf

MODULE_NAME = Camera boxes to world
MODULE_COMMENT = Find global coordinates of bounding boxes in camera image.

LINK = g++

CFLAGS += -Wall
CXXFLAGS += -Wall

IFLAGS += 
LFLAGS += -lglobal -lipc -lvelodyne_interface -lbumblebee_basic_interface -lparam_interface `pkg-config --libs opencv` \
          -lvelodyne_camera_calibration \
          -L$(CARMEN_HOME)/sharedlib/libtf/src -ltf -lrt \
          -lBulletCollision -lBulletDynamics -lBulletSoftBody -lLinearMath \
		  -lboost_thread-mt -lboost_signals -lboost_system \
		  -lcamera_boxes_to_world

SOURCES = camera_boxes_to_world.cpp show_boxes.cpp

PUBLIC_INCLUDES = camera_boxes_to_world.h
PUBLIC_LIBRARIES = libcamera_boxes_to_world.a
PUBLIC_BINARIES = 

TARGETS = libcamera_boxes_to_world.a show_boxes

libcamera_boxes_to_world.a: camera_boxes_to_world.o

show_boxes: show_boxes.o

include ../Makefile.rules
