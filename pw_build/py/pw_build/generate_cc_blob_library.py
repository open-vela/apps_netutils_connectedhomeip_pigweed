# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Outputs the contents of blobs as a hard-coded arrays in a C++ library."""

import argparse
import itertools
import json
from pathlib import Path
from string import Template
import textwrap
from typing import (
    Any,
    Generator,
    Iterable,
    NamedTuple,
    Optional,
    Sequence,
    Tuple,
)

COMMENT = f"""\
// This file was generated by {Path(__file__).name}.
//
// DO NOT EDIT!
//
// This file contains declarations for byte arrays created from files during the
// build. The byte arrays are constant initialized and are safe to access at any
// time, including before main().
//
// See https://pigweed.dev/pw_build/#pw-cc-blob-library for details.
"""

HEADER_PREFIX = COMMENT + textwrap.dedent(
    """\
    #pragma once

    #include <array>
    #include <cstddef>

    """
)

SOURCE_PREFIX_TEMPLATE = Template(
    COMMENT
    + textwrap.dedent(
        """\

        #include "$header_path"

        #include <array>
        #include <cstddef>

        #include "pw_preprocessor/compiler.h"

        """
    )
)

NAMESPACE_OPEN_TEMPLATE = Template('namespace ${namespace} {\n')

NAMESPACE_CLOSE_TEMPLATE = Template('\n}  // namespace ${namespace}\n')

BLOB_DECLARATION_TEMPLATE = Template(
    '\nextern const std::array<std::byte, ${size_bytes}> ${symbol_name};\n'
)

LINKER_SECTION_TEMPLATE = Template('PW_PLACE_IN_SECTION("${linker_section}")\n')

BLOB_DEFINITION_MULTI_LINE = Template(
    '\n${alignas}'
    '${section_attr}constexpr std::array<std::byte, ${size_bytes}>'
    ' ${symbol_name}'
    ' = {\n${bytes_lines}\n};\n'
)

BYTES_PER_LINE = 4


class Blob(NamedTuple):
    symbol_name: str
    file_path: Path
    linker_section: Optional[str]
    alignas: Optional[str] = None

    @staticmethod
    def from_dict(blob_dict: dict) -> 'Blob':
        return Blob(
            blob_dict['symbol_name'],
            Path(blob_dict['file_path']),
            blob_dict.get('linker_section'),
            blob_dict.get('alignas'),
        )


def parse_args() -> dict:
    """Parse arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        '--blob-file',
        type=Path,
        required=True,
        help=('Path to json file containing the list of blobs ' 'to generate.'),
    )
    parser.add_argument(
        '--header-include',
        required=True,
        help='Path to use in #includes for the header',
    )
    parser.add_argument(
        '--out-source',
        type=Path,
        required=True,
        help='Path at which to write .cpp file',
    )
    parser.add_argument(
        '--out-header',
        type=Path,
        required=True,
        help='Path at which to write .h file',
    )
    parser.add_argument(
        '--namespace',
        type=str,
        required=False,
        help=('Optional namespace that blobs will be scoped' 'within.'),
    )

    return vars(parser.parse_args())


def split_into_chunks(
    data: Iterable[Any], chunk_size: int
) -> Generator[Tuple[Any, ...], None, None]:
    """Splits an iterable into chunks of a given size."""
    data_iterator = iter(data)
    chunk = tuple(itertools.islice(data_iterator, chunk_size))
    while chunk:
        yield chunk
        chunk = tuple(itertools.islice(data_iterator, chunk_size))


def header_from_blobs(
    blobs: Iterable[Blob], namespace: Optional[str] = None
) -> str:
    """Generate the contents of a C++ header file from blobs."""
    lines = [HEADER_PREFIX]
    if namespace:
        lines.append(NAMESPACE_OPEN_TEMPLATE.substitute(namespace=namespace))
    for blob in blobs:
        data = blob.file_path.read_bytes()
        lines.append(
            BLOB_DECLARATION_TEMPLATE.substitute(
                symbol_name=blob.symbol_name, size_bytes=len(data)
            )
        )
    if namespace:
        lines.append(NAMESPACE_CLOSE_TEMPLATE.substitute(namespace=namespace))

    return ''.join(lines)


def array_def_from_blob_data(blob: Blob, blob_data: bytes) -> str:
    """Generates an array definition for the given blob data."""
    if blob.linker_section:
        section_attr = LINKER_SECTION_TEMPLATE.substitute(
            linker_section=blob.linker_section
        )
    else:
        section_attr = ''

    byte_strs = ['std::byte{{0x{:02X}}}'.format(b) for b in blob_data]

    lines = []
    for byte_strs_for_line in split_into_chunks(byte_strs, BYTES_PER_LINE):
        bytes_segment = ', '.join(byte_strs_for_line)
        lines.append(f'    {bytes_segment},')

    return BLOB_DEFINITION_MULTI_LINE.substitute(
        section_attr=section_attr,
        alignas=f'alignas({blob.alignas}) ' if blob.alignas else '',
        symbol_name=blob.symbol_name,
        size_bytes=len(blob_data),
        bytes_lines='\n'.join(lines),
    )


def source_from_blobs(
    blobs: Iterable[Blob], header_path: str, namespace: Optional[str] = None
) -> str:
    """Generate the contents of a C++ source file from blobs."""
    lines = [SOURCE_PREFIX_TEMPLATE.substitute(header_path=header_path)]
    if namespace:
        lines.append(NAMESPACE_OPEN_TEMPLATE.substitute(namespace=namespace))
    for blob in blobs:
        data = blob.file_path.read_bytes()
        lines.append(array_def_from_blob_data(blob, data))
    if namespace:
        lines.append(NAMESPACE_CLOSE_TEMPLATE.substitute(namespace=namespace))

    return ''.join(lines)


def load_blobs(blob_file: Path) -> Sequence[Blob]:
    with blob_file.open() as blob_fp:
        return [Blob.from_dict(blob) for blob in json.load(blob_fp)]


def main(
    blob_file: Path,
    header_include: str,
    out_source: Path,
    out_header: Path,
    namespace: Optional[str] = None,
) -> None:
    blobs = load_blobs(blob_file)

    out_header.parent.mkdir(parents=True, exist_ok=True)
    out_header.write_text(header_from_blobs(blobs, namespace))

    out_source.parent.mkdir(parents=True, exist_ok=True)
    out_source.write_text(source_from_blobs(blobs, header_include, namespace))


if __name__ == '__main__':
    main(**parse_args())
