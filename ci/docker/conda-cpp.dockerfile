# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

ARG repo
ARG arch
FROM ${repo}:${arch}-conda

COPY ci/scripts/install_minio.sh /arrow/ci/scripts
RUN /arrow/ci/scripts/install_minio.sh latest /opt/conda

# Unless overridden use Python 3.10
# Google GCS fails building with Python 3.11 at the moment.
ARG python=3.10

# install the required conda packages into the test environment
# use `mold` to work around issues with GNU `ld` (GH-47015).
COPY ci/conda_env_cpp.txt \
     ci/conda_env_gandiva.txt \
     /arrow/ci/
RUN mamba install -q -y \
        --file arrow/ci/conda_env_cpp.txt \
        --file arrow/ci/conda_env_gandiva.txt \
        compilers \
        doxygen \
        libnuma \
        mold \
        python=${python} \
        valgrind && \
    mamba clean --all --yes

# We want to install the GCS testbench using the Conda base environment's Python,
# because the test environment's Python may later change.
ENV PIPX_BASE_PYTHON=/opt/conda/bin/python3
COPY ci/scripts/install_gcs_testbench.sh /arrow/ci/scripts
RUN /arrow/ci/scripts/install_gcs_testbench.sh default

# Ensure npm, node and azurite are on path. npm and node are required to install azurite, which will then need to
# be on the path for the tests to run.
ENV PATH=/opt/conda/envs/arrow/bin:$PATH

COPY ci/scripts/install_azurite.sh /arrow/ci/scripts/
RUN /arrow/ci/scripts/install_azurite.sh

COPY ci/scripts/install_sccache.sh /arrow/ci/scripts/
RUN /arrow/ci/scripts/install_sccache.sh unknown-linux-musl /usr/local/bin

# Firebolt: trimmed feature set (cpp parquet/orc focus); the rest disabled.
ENV ARROW_ACERO=OFF \
    ARROW_AZURE=OFF \
    ARROW_BUILD_TESTS=ON \
    ARROW_DATASET=OFF \
    ARROW_DEPENDENCY_SOURCE=CONDA \
    ARROW_FLIGHT=OFF \
    ARROW_FLIGHT_SQL=OFF \
    ARROW_GANDIVA=OFF \
    ARROW_GCS=OFF \
    ARROW_HDFS=OFF \
    ARROW_HOME=$CONDA_PREFIX \
    ARROW_JEMALLOC=ON \
    ARROW_MIMALLOC=OFF \
    ARROW_ORC=ON \
    ARROW_PARQUET=ON \
    ARROW_S3=OFF \
    ARROW_S3_MODULE=OFF \
    ARROW_SUBSTRAIT=OFF \
    ARROW_USE_CCACHE=ON \
    ARROW_USE_MOLD=ON \
    ARROW_WITH_BROTLI=ON \
    ARROW_WITH_BZ2=ON \
    ARROW_WITH_LZ4=ON \
    # Blocked on https://issues.apache.org/jira/browse/ARROW-15066
    ARROW_WITH_OPENTELEMETRY=OFF \
    ARROW_WITH_SNAPPY=ON \
    ARROW_WITH_ZLIB=ON \
    ARROW_WITH_ZSTD=ON \
    CMAKE_CXX_STANDARD=20 \
    GTest_SOURCE=BUNDLED \
    PARQUET_BUILD_EXAMPLES=OFF \
    PARQUET_BUILD_EXECUTABLES=OFF \
    PARQUET_HOME=$CONDA_PREFIX
