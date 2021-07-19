
CC= g++
# IROOT directory based on installed distribution tree (not archive/development tree). 
# IROOT=../..
IROOT=/usr/dalsa/GigeV

SRC_DIR = src
OBJ_DIR = obj

EXE = visss-data-acquisition
EXE2 = visss-data-acquisition-dryrun

SRC = $(wildcard $(SRC_DIR)/*.cpp)
OBJ = $(SRC:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)

gitTag    := \"$(shell git describe  --always)\"
gitBranch := \"$(shell git rev-parse --abbrev-ref HEAD)\"

#
# Get the configured include defs file.
# (It gets installed to the distribution tree).
ifeq ($(shell if test -e src/archdefs.mk; then echo exists; fi), exists)
	include src/archdefs.mk
else
 # Force an error
 $(error	archdefs.mk file not found. It gets configured on installation ***)
endif

INC_PATH = -I. -I$(IROOT)/include -I$(IROOT)/examples/common $(INC_GENICAM) 
openCV = $(shell pkg-config --cflags --libs opencv4)

print-%  : ; @echo $* = $($*)

DEBUGFLAGS = -O2

#
# Conditional definitions for the common demo files
# (They depend on libraries installed in the system).
#
include src/commondefs.mk

CXX_COMPILE_OPTIONS = -c $(DEBUGFLAGS) -std=c++2a -DPOSIX_HOSTPC -D_REENTRANT -fno-for-scope \
			-Wall -Wno-parentheses -Wno-missing-braces -Wno-unused-but-set-variable \
			-Wno-unknown-pragmas -Wno-cast-qual -Wno-unused-function -Wno-unused-label -DGIT_BRANCH=$(gitBranch) \
			-DGIT_TAG=$(gitTag) #-Og -g

C_COMPILE_OPTIONS= $(DEBUGFLAGS) -fhosted -Wall -Wno-parentheses -Wno-missing-braces \
		   	-Wno-unknown-pragmas -Wno-cast-qual -Wno-unused-function -Wno-unused-label -Wno-unused-but-set-variable


LCLLIBS=  $(COMMONLIBS) -lpthread  -lm -lpcap

# %.o : %.cpp
# 	$(CC) -I. $(INC_PATH) $(openCV) $(CXX_COMPILE_OPTIONS) $(COMMON_OPTIONS)  -I/home/mmaahn/ffmpeg_build/include/ -L/home/mmaahn/ffmpeg_build/lib/ -lavcodec -lavutil -c $< -o $@

# %.o : %.c
# 	$(CC) -I. $(INC_PATH) $(openCV) $(C_COMPILE_OPTIONS) $(COMMON_OPTIONS) -lavcodec -lavutil -c $< -o $@

# OBJS1= src/visss-data-acquisiton.o \
# 	src/opencv_writer.o


.PHONY: all clean

all: $(EXE) $(EXE2)

install:
	mv $(EXE) ../bin


# $(EXE): $(OBJ)
# 	$(CC) -g  $^ $(LCLLIBS) $(openCV)  $(GENICAM_LIBS) -L$(ARCHLIBDIR) -lstdc++ -o $@
# 	/usr/bin/sudo /sbin/setcap cap_sys_nice,cap_net_raw+ep $(EXE)

# $(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
# 	$(CC) -I. $(INC_PATH) $(openCV) $(CXX_COMPILE_OPTIONS) $(COMMON_OPTIONS)  -I/home/mmaahn/ffmpeg_build/include/ -L/home/mmaahn/ffmpeg_build/lib/ -lavcodec -lavutil -c $< -o $@


$(EXE): $(OBJ_DIR)/visss-data-acquisition.o
	$(CC) -g $^ $(LCLLIBS) -L/usr/local/lib -lGevApi  -lCorW32  -lavcodec -lavutil $(openCV)  $(GENICAM_LIBS) -L$(ARCHLIBDIR) -lstdc++ -o $@
	/usr/bin/sudo /sbin/setcap cap_sys_nice,cap_net_raw+ep $(EXE)
	/usr/bin/sudo /sbin/setcap cap_net_raw+eip $(EXE)

$(OBJ_DIR)/visss-data-acquisition.o: $(SRC_DIR)/visss-data-acquisition.cpp
	$(CC) -I. $(INC_PATH) $(openCV) $(CXX_COMPILE_OPTIONS) $(COMMON_OPTIONS)  -c $< -o $@

$(EXE2): $(OBJ_DIR)/visss-data-acquisition-dryrun.o
	$(CC) -g $^ $(LCLLIBS) $(openCV) -lavcodec -lavutil  -lstdc++ -o $@

$(OBJ_DIR)/visss-data-acquisition-dryrun.o: $(SRC_DIR)/visss-data-acquisition-dryrun.cpp
	$(CC) -I. $(openCV) $(CXX_COMPILE_OPTIONS) $(COMMON_OPTIONS)  -c $< -o $@

clean:
# 	$(RM) $(OBJ)
	$(RM) $(OBJ_DIR)/*
# all: src/visss-data-acquisiton


# visss-data-acquisiton : $(OBJS1)
# 	$(CC) -g -o visss-data-acquisiton $(OBJS1) $(LCLLIBS) $(openCV)  $(GENICAM_LIBS) -L$(ARCHLIBDIR) -lstdc++

# clean:
# 	rm *.o visss-data-acquisiton cv2ff restore_nano_data_demo


