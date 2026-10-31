FROM gcc:14-bookworm AS builder
WORKDIR /src
COPY b_index.cpp .
RUN g++ -std=c++20 -O3 -flto -march=native -mavx2 -DNDEBUG -pipe -o /b_index b_index.cpp

FROM python:3.13-slim@sha256:d98cc29d3266e535dd9da1099688d9cb3ca4753813571e071763d17725460962
COPY . /app
COPY --from=builder /b_index /app/b_index_cpp
WORKDIR /app
ENTRYPOINT ["python3", "/app/run.py"]
