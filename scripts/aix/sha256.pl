#!/usr/bin/perl
# Copyright (c) 2026 Raman Dzehtsiar
# SPDX-License-Identifier: MIT

#
# Purpose:
#   Calculate portable SHA-256 digests in the same output format on AIX and
#   Linux, avoiding platform-specific sha256 command names and options.
#
# Usage:
#   Pass one or more files from the repository root:
#     perl scripts/aix/sha256.pl CMakeLists.txt src/common/usfs_config.h

use strict;
use warnings;
use Digest::SHA;

@ARGV or die "usage: $0 FILE...\n";
for my $path (@ARGV) {
    open my $input, '<', $path or die "$path: $!\n";
    binmode $input;
    my $digest = Digest::SHA->new(256);
    $digest->addfile($input);
    close $input or die "$path: $!\n";
    print $digest->hexdigest, "  ", $path, "\n";
}
