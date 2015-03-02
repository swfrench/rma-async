########
# config

# compilation
CXX = mpicxx
CXXFLAGS = -g -O2 --std=c++11 -fno-PIE

# linking
LD = mpicxx
LDFLAGS = -fno-PIE
LDLIBS = -L$L -lrma_async

# library generation
AR = ar cru
RANLIB = ranlib

# maximum number of function parameters to support
NUM_PARAMS_GEN=10

###############
# static config

# auto-generated header files
GEN = gen.async_task_data.hpp gen.async_task_run.hpp gen.async_task_invoke.hpp

# paths
O = obj
L = lib
B = bin

# object list
OBJ = $O/async.o

#############
# main target

LIB = $L/librma_async.a

all : $(LIB)

$(LIB) : $L $O $(OBJ)
	$(AR) $@ $(OBJ)
	$(RANLIB) $@

#####################
# build product paths

$L :
	mkdir -p $@

$O :
	mkdir -p $@

$B :
	mkdir -p $@

#####################
# general compilation

$O/%.o : %.cpp async.hpp $(GEN)
	$(CXX) $(CXXFLAGS) -c $< -o $@

########################
# auto-generated headers

$(GEN) : auto_gen.sh
	@echo "generating header files ..."
	@./auto_gen.sh $(NUM_PARAMS_GEN)

#########
# example

.PHONY : example
example : $O $B $B/example.x

$B/example.x : $O/example.o $(LIB)
	$(LD) $(LDFLAGS) $O/example.o $(LDLIBS) -o $@

######
# docs

.PHONY : docs
docs : $(GEN) doc/config
	doxygen doc/config

#########
# cleanup

.PHONY : clean
clean:
	rm -rf $O

.PHONY: distclean
distclean: clean
	rm -rf $L $B $(GEN) doc/generated
