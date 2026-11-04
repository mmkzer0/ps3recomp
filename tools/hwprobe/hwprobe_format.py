#!/usr/bin/env python3
"""Reader of the header every hwprobe result file starts with (common/hwprobe_result.c).

    HWPROBE,<format version>
    HDR,<key>,<value>          (one row per key)

A file whose first row is not HWPROBE,<known version> is rejected with FormatError. That covers
a file from a newer toolkit (unknown version) and a file that is not a hwprobe result at all.
Standard library only.
"""

SUPPORTED_VERSIONS = (1,)

# Keys every version-1 header carries (README.md, "Result header").
REQUIRED_KEYS = ("probe", "probe_version", "git_describe", "source_sha256", "fw_version", "fw_type",
                 "console_model", "console_info_source", "config", "tbfreq")


class FormatError(ValueError):
    """The file is not a result file this reader can interpret."""


def parse_header(lines):
    """Parse the header at the start of `lines` (iterable of text lines).

    Returns (header dict, number of lines consumed). The dict has the HDR keys plus
    "format_version" (int); "tbfreq" is converted to int. Blank lines and '#' comments before
    the first row are skipped and counted as consumed.
    """
    lines = list(lines)
    pos = 0
    while pos < len(lines) and (not lines[pos].strip() or lines[pos].lstrip().startswith("#")):
        pos += 1
    if pos >= len(lines):
        raise FormatError("empty result file")
    first = lines[pos].strip().split(",")
    if first[0] != "HWPROBE":
        raise FormatError(f"not a hwprobe result file: first row is {lines[pos].strip()[:60]!r}, expected 'HWPROBE,<version>' "
                          "(a file written before the versioned header existed is not supported)")
    try:
        version = int(first[1])
    except (IndexError, ValueError):
        raise FormatError(f"bad format version row {lines[pos].strip()[:60]!r}")
    if version not in SUPPORTED_VERSIONS:
        raise FormatError(
            "unsupported hwprobe format version {} (this reader supports {}); "
            "use the analysis tools from the toolkit revision that produced the file".format(
                version, ", ".join(str(v) for v in SUPPORTED_VERSIONS)))
    pos += 1
    hdr = {"format_version": version}
    while pos < len(lines) and lines[pos].startswith("HDR,"):
        parts = lines[pos].rstrip("\r\n").split(",", 2)
        if len(parts) != 3:
            raise FormatError(f"malformed header row {lines[pos].strip()[:60]!r}")
        hdr[parts[1]] = parts[2]
        pos += 1
    missing = [k for k in REQUIRED_KEYS if k not in hdr]
    if missing:
        raise FormatError("header lacks required keys: {}".format(", ".join(missing)))
    try:
        hdr["tbfreq"] = int(hdr["tbfreq"])
    except ValueError:
        raise FormatError("header tbfreq {!r} is not an integer".format(hdr["tbfreq"]))
    return hdr, pos
