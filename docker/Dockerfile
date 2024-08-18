FROM ubuntu:latest as buildenv
RUN apt update
RUN apt install -y build-essential git pkg-config
RUN apt install -y libfuse-dev fuse
ADD ./* /src/
WORKDIR /src
RUN make


FROM us-docker.pkg.dev/android-emulator-268719/images/30-google-x64:30.1.2
# FROM docker.io/budtmo/docker-android:emulator_12.0
# RUN apt update

# RUN apt install -y fuse libfuse-dev

# ADD ./* /src/
ADD ./tests/* /src/tests/
ADD ./docker/* /src/docker/

RUN chmod +x /src/docker/*.sh

COPY --from=buildenv /src/adbfs /usr/bin/adbfs
RUN chmod +x /usr/bin/adbfs

WORKDIR /src
# CMD /src/docker/run-docker-test.sh
ENTRYPOINT [ "sudo", "bash", "/src/docker/run-test.sh" ]
# ENTRYPOINT [ "bash" ]