FROM ubuntu:20.04
LABEL Description="Pushpin Build environment"

SHELL ["/bin/bash", "-c"]

WORKDIR /pushpin

COPY . .

RUN apt-get install apt-transport-https software-properties-common
RUN echo deb https://fanout.jfrog.io/artifactory/debian fanout-focal main | tee /etc/apt/sources.list.d/fanout.list
RUN apt-key adv --keyserver hkp://keyserver.ubuntu.com:80 --recv-keys EA01C1E777F95324
RUN apt-get update
RUN apt-get install -y git
RUN apt-get install pkg-config rustc cargo qtbase5-dev libzmq3-dev zurl

RUN ./configure \
    && cargo build --release \
    && make \
    && make install

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