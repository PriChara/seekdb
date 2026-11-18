ARG PYTHON_IMAGE=python:3.11.14-slim-bookworm
FROM gcc:14-bookworm AS builder
WORKDIR /src
COPY vector_scan.c .
RUN gcc -std=c11 -O3 -Wall -Wextra -Werror -shared -fPIC -o vector_scan.so vector_scan.c

FROM ${PYTHON_IMAGE}
WORKDIR /app
COPY minivectordb.py /app/minivectordb.py
COPY --from=builder /src/vector_scan.so /app/vector_scan.so
ENV PYTHONUTF8=1 PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["python3", "/app/minivectordb.py"]
