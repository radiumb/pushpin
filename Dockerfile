FROM ubuntu:20.04
LABEL Description="Pushpin Build environment"

SHELL ["/bin/bash", "-c"]

WORKDIR /pushpin

COPY . .

RUN apt-get update
RUN apt-get install -y git wget
RUN apt-get install build-essential -y --no-install-recommends
RUN DEBIAN_FRONTEND=noninteractive TZ=Etc/UTC apt-get -y install tzdata
RUN apt-get install -y pkg-config rustc cargo qtbase5-dev libzmq3-dev zurl

RUN wget http://ftp.de.debian.org/debian/pool/main/r/rust-condure/condure_1.1.0-1+b2_amd64.deb
RUN apt-get install ./condure_1.1.0-1+b2_amd64.deb

RUN git clone https://github.com/fanout/common.git src/corelib/common
RUN git clone https://github.com/jkarneges/qzmq.git src/corelib/qzmq

RUN ./configure \
    && cargo build --release \
    && make \
    && make install

CMD ["pushpin", "--config /etc/pushpin/pushpin.conf"]

# Expose ports.
# - 7999: HTTP port to forward on to the app
# - 5560: ZMQ PULL for receiving messages
# - 5561: HTTP port for receiving messages and commands
# - 5562: ZMQ SUB for receiving messages
# - 5563: ZMQ REP for receiving commands
EXPOSE 7999
EXPOSE 5560
EXPOSE 5561
EXPOSE 5562
EXPOSE 5563
EXPOSE 9001