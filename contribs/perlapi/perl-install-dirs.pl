#!/usr/bin/perl
# Return the Perl search paths used by the contributed module installation.
# This file is part of Slurm, distributed under GNU GPL version 2 or later.

use strict;
use warnings;
use Config;
use ExtUtils::MakeMaker;

my $prefix = shift @ARGV;
die "Usage: $0 PREFIX\n" if !defined($prefix) || @ARGV;

if (($ENV{SLURM_PERL_INSTALLDIRS} || 'site') eq 'vendor') {
    # Keep the existing distribution-packaging convention.
    print join(' ', @Config{qw(installvendorlib installvendorarch)});
    exit;
}

# Use the same prefix translation as MakeMaker. In particular, macOS's
# installsitearch need not be beneath siteprefix, so string stripping is wrong.
# Initialize only installation paths; do not scan sources or write a Makefile.
my $make = bless {
    PREFIX => $prefix,
    INSTALLDIRS => 'site',
    ARGS => { PREFIX => $prefix },
}, 'MM';
$make->init_INSTALL_from_PREFIX;
my @paths;
for my $key (qw(INSTALLSITELIB INSTALLSITEARCH)) {
    my $path = $make->{$key};
    $path =~ s/\$\((?:SITE)?PREFIX\)/$prefix/g;
    die "Unresolved MakeMaker installation path: $path\n" if $path =~ /\$\(/;
    push @paths, $path;
}
print join(' ', @paths);
