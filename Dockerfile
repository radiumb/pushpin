FROM ubuntu:20.04
LABEL Description="Pushpin Build environment"

SHELL ["/bin/bash", "-c"]

WORKDIR /pushpin

COPY . .

RUN git submodule init && git submodule update \
    sudo apt-get install pkg-config rustc cargo qtbase5-dev libzmq3-dev condure zurl
    ./configure \
    sudo cargo build --release \
    sudo make \
    sudo make install

CMD ["sudo pushpin", "--verbose"]

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