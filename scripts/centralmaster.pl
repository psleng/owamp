#!/usr/bin/perl -w

# File: centralmaster.pl
# Author: Anatoly Karp, Internet2 2002
# $Id$

# This script runs on the central host. It validates the data
# as it comes, and runs owdigest on it.

use strict;
use Socket;
use Sys::Hostname;
use Fcntl;
use Digest::MD5;

use GDBM_File;

use constant DEBUG => 1;
use constant TMP_SECRET => 'abcdefgh12345678';
use constant LOW => 0;
use constant HIGH => 1;

use constant INVALID => 0;
use constant VALID => 1;

use constant VERBOSE => 1;

### Configuration section.
my $server_port = 2345;

# $top_dir contains the hierarchy of receiver directories
my $top_dir = '/home/karp/projects/owamp/datadep';

# path to the 'owdigest' executable.
my $digest_path = '/home/karp/projects/owamp/owdigest/owdigest';

# this is the file containing the secret to hash timestamps with.
my $passwd_file = '/home/karp/projects/owamp/etc/owampd.passwd';

# this is a log file with liveness reports from nodes
my $log_file = "$top_dir/liveness.dat";
### End of configuration section.

chdir $top_dir or die "Could not chdir to $top_dir: $!";

open(PASSWD, "<$passwd_file") or die "Could not open $passwd_file: $!";
my $secret = <PASSWD>;
unless ($secret) {
    warn "Could not read secret from $passwd_file";
    $secret = TMP_SECRET;
    die "Cannot function without a secret!" unless DEBUG;
}
chomp $secret;
close PASSWD;

# Initialize data structures for keeping track of updates.
my %live_times;   # this hash keeps track of intervals [start_time, cur_time]
                  # ordered by start_time in the increasing order

# Open the database and read in its contents.
my %live_db;
tie %live_db, 'GDBM_File', $log_file, &GDBM_WRCREAT, 0640;
for (keys %live_db) {
    $live_times{$_} = [];
    my @points = split ':', $live_db{$_};
    my $even = 1;
    my $low;
    for my $point (@points) {
	if ($even) {
	    $low = $point;
	} else {
	    push @{$live_times{$_}}, [$low, $point];
	}
	$even = 1 - $even;
    }
}

socket(my $server, PF_INET, SOCK_DGRAM, getprotobyname('udp'));
my $iaddr = gethostbyname(hostname());
my $proto = getprotobyname('udp');
my $paddr = sockaddr_in($server_port, $iaddr);

socket($server, PF_INET, SOCK_DGRAM, $proto)   || die "socket: $!";
bind($server, $paddr)                          || die "bind: $!";

# The server will only attempt to digest files if it keeps
# getting current time updates from hosts. Even as data keeps
# coming in through ssh nothing can be done with until it's
# validated - which is only done via timestamps. 

my $buf;
while (1) {
    if (my $srcaddr = recv($server, $buf, 128, 0)) {
	next unless $buf;
	my ($port, $addr) = sockaddr_in($srcaddr);
	my $src = inet_ntoa($addr);
	my ($start_time, $cur_time, $hashed) = split /\./, $buf;
	my $plain = "$start_time.$cur_time.$secret";
	warn "received $plain" if VERBOSE;
	unless (Digest::MD5::md5_hex("$start_time.$cur_time.$secret") 
		eq $hashed) {
	    warn "DEBUG: hash mismatch\n";
	    warn "\$plain = $plain\n";
	    next;
	}

	# Update the list of live intervals, or initialize it if there's none.
	# Similarly with the log database.
	if (exists $live_times{$src}) {
	    my $final = $#{$live_times{$src}};
	    if ($start_time > $live_times{$src}[$final][0]) {
		print "DEBUG: received new start time: $start_time\n" if DEBUG;
		push @{$live_times{$src}}, [$start_time, $cur_time];
		$live_db{$src} .= ":$start_time:$cur_time";
	    }

	    for (my $i = $final; $i >= 0; $i--) {
		if (DEBUG) {
		    warn "DEBUG: start time = $start_time\n";
		    warn "DEBUG: time[$i][0]=@{[$live_times{$src}[$i][0]]}\n";
		    warn "DEBUG: time[$i][1]=@{[$live_times{$src}[$i][1]]}\n";
		}

		if ($start_time == $live_times{$src}[$i][0]) {
		    print "DEBUG: matched $start_time\n" if DEBUG;

		    if ($cur_time > $live_times{$src}[$i][1]) { # grow interval

			if (DEBUG) {
			    warn "DEBUG: growing the upper boundary...\n";
			    print "\t",
				    "$live_times{$src}[$i][1] --> $cur_time\n";
			}

			$live_times{$src}[$i][1] = $cur_time;
			my @intervals = split /:/, $live_db{$src};
			$intervals[2*$i + 1] = $cur_time;
			$live_db{$src} = join ':', @intervals;
		    }
		}
	    }
	} else {
	    @{$live_times{$src}} = ();
	    push @{$live_times{$src}}, [$start_time, $cur_time];
	    $live_db{$src} = "$start_time:$cur_time";
	}

	# When get a new update for a host - process all files for which
	# it is the sender. Then can return back into the loop - since there's
	# no more information to act upon

	opendir(DIR, "$top_dir") || die "Cannot opendir $top_dir: $!";
	my @receivers = grep {$_ !~ /^\./ && -d $_} readdir(DIR);
	closedir DIR;

	for my $recv (@receivers) {
	    my $dirpath = "$top_dir/$recv/$src";
	    next unless -d $dirpath;
	    opendir(OWPDATA, "$dirpath") 
		    or die "Could not opendir $dirpath: $!";
	    my @files = grep {-f $_} readdir(DIR);
	    closedir OWPDATA;
	    warn "top_dir=$top_dir\nrecv=$recv\nsrc=$src\nfiles: @files \n"
		    if DEBUG;

	  FILE:
	    for (@files) {
		my $name = $_;
		next unless ($name =~ s/\.owp$//);
		my ($start, $end) = split /_/, $name;
		warn "start=$start    end=$end\n" if DEBUG;
		my $fullpath = "$dirpath/$_";

		next unless (exists $live_times{$src}); # status unknown
		my $final = $#{$live_times{$src}};

		if ($end > $live_times{$src}[$final][HIGH]
		    || $start < $live_times{$src}[0][LOW]) {
		    warn "file $fullpath: status unknown: skipping\n" if DEBUG;
		    next;
		}

		if (contains($start, $end, $live_times{$src}[0][HIGH])
		    || contains($start, $end, $live_times{$src}[$final][LOW])){
		    warn "file $fullpath invalid: archiving\n" if DEBUG;
		    archive($fullpath, VALID);
		    next;
		}

		for (my $i = 1; $i <= $final - 1; $i++) {
		    if (contains($start, $end, $live_times{$src}[$i][LOW])
			||contains($start, $end, $live_times{$src}[$i][HIGH])){
			warn "file $fullpath invalid: archiving\n" if DEBUG;
			archive($fullpath, INVALID);
			next FILE;
		    }
		}

		warn "validated file $fullpath ..digesting + archiving now..\n"
			if DEBUG;
		system("$digest_path $fullpath $fullpath.digest > /dev/null");
		archive($fullpath, VALID);
	    }
	}
	warn "DEBUG: no more dirs - going back into recv loop\n\n" if DEBUG;
	sleep 5;
    }
}

# Archiving function - currently unlink.
sub archive {
    my ($file, $type) = @_;

    if ($type == VALID) { # temporary  measure
	unlink $file or warn "Could not unlink $file: $!";
	return;
    }

    # type == INVALID
    unlink $file or warn "Could not unlink $file: $!";
}

# Return 1 if the interval [$low, $high] contains $point, and 0 otherwise.
sub contains {
    my ($low, $high, $point);
    return ($low <= $point && $point <= $high)? 1 : 0;
}