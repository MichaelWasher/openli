FROM debian:latest

# Install the general docker compiler / builidng dependencies
RUN apt update && apt install -y\
    lsb-release                 \
    curl                        \
    build-essential             \
    libtool                     \
    automake                    \
    autotools-dev               \
    apt-transport-https

# Install the OpenLI Specific Libaries
RUN apt update && apt install -y\
    libyaml-dev                 \
    libosip2-dev                \
    uthash-dev                  \
    libzmq3-dev                 \
    libjudy-dev                 \
    libgoogle-perftools-dev

# Need WAND repo for this

RUN echo "deb https://packages.wand.net.nz $(lsb_release -sc) main" | tee /etc/apt/sources.list.d/wand.list
RUN curl https://packages.wand.net.nz/keyring.gpg -o /etc/apt/trusted.gpg.d/wand.gpg

RUN apt update && apt install -y\
    libtrace4-dev               \
    libtrace4                   \
    libwandder1                 \
    libwandder1-dev             
   
CMD /bin/bash


