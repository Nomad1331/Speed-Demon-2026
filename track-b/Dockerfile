# Two-stage GCC 16 allowlisted build for Speed Demon 2026 Track B
FROM gcc:16@sha256:43978d7710a803d1c3d32f4117c1528ec7285a4ce2c0a51e982ec1a6e3d87d95 AS build
WORKDIR /src
COPY main.cpp .
RUN g++ -std=c++20 -O3 -march=x86-64-v3 -flto -pthread -static-libgcc -static-libstdc++ main.cpp -o solution

FROM debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad
WORKDIR /app
COPY --from=build /src/solution /app/solution
ENTRYPOINT ["/app/solution", "--data", "/data", "--out", "/out"]
