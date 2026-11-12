ARG PYTHON_IMAGE=python:3.11.14-slim-bookworm
FROM ${PYTHON_IMAGE}
WORKDIR /app
COPY minivectordb.py /app/minivectordb.py
ENV PYTHONUTF8=1 PYTHONDONTWRITEBYTECODE=1
ENTRYPOINT ["python3", "/app/minivectordb.py"]
