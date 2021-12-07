# pass BASE from command line or edit this line
# dependcy projects are expected to be under the BASE
# rippled and boost must be built separately
BASE=${HOME}/Documents/Projects
RIPPLED=${BASE}/rippled
BOOST=${BASE}/boost_1_70_0
SECP256K1=${RIPPLED}/src/secp256k1/include
# https://github.com/HowardHinnant/date.git
DATE=${BASE}/date/include
FLAGS=--std=c++17 -I${RIPPLED}/src -I${RIPPLED}/src/ripple -I${BOOST} -I${DATE} -I${SECP256K1} \
	-L${RIPPLED}/build -L/${BOOST}/stage/lib
SRC=peermon.cpp ripple.pb.cc base58.c xd.c sha-256.c
HDR=libbase58.h xd.h stlookup.h sha-256.h
RIPPLE_LIBS=-lxrpl_core -led25519-donna -lsecp256k1
BOOST_LIBS=-lboost_chrono -lboost_container -lboost_context -lboost_coroutine -lboost_date_time \
	-lboost_filesystem -lboost_program_options -lboost_regex -lboost_system -lboost_thread -lpthread
LIBS=-lsecp256k1 -lsodium -lssl -lcrypto -lprotobuf -fpermissive

peermon: ${SRC} ${HDR}
	g++ -g -o peermon ${FLAGS} ${SRC} ${LIBS} ${RIPPLE_LIBS} ${BOOST_LIBS} -Bstatic

ripple.pb.cc:
	protoc --cpp_out=. ripple.proto


