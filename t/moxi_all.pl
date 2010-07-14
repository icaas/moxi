#!/usr/bin/perl

# Run this main test driver program from the project's
# top directory, which has t as a subdirectory.
#
my $exe = "./moxi-debug";

croak("moxi binary doesn't exist.  Haven't run 'make' ?\n") unless -e $exe;
croak("moxi binary not executable\n") unless -x _;

sub go {
  my ($topology, $protocol) = @_;
  print "------------------------------------\n";
  print "testing $topology $protocol\n";
  my $result = system("./t/moxi.pl $topology $protocol");
  if ($result != 0) {
    print("fail moxi.pl $topology $protocol test\n");
    exit $result;
  }
}

# Ascii protocol compatibility tests.
#
go('simple',   'ascii');
go('chain',    'ascii');
go('fanout',   'ascii');
go('fanoutin', 'ascii');

# Binary protocol compatibility tests.
#
go('simple', 'binary');
go('fanout', 'binary');

print "------------------------------------\n";

# Fork moxi-debug for moxi-specific testing.
#
my $childargs =
      " -z ./t/moxi_mock.cfg".
      " -p 0 -U 0 -v -t 1 -Z \"downstream_max=1,downstream_protocol=ascii\"";
if ($< == 0) {
   $childargs .= " -u root";
}
my $childpid = fork();

unless ($childpid) {
    setpgrp();
    exec "$exe $childargs";
    exit; # never gets here.
}
setpgrp($childpid, $childpid);

my $result = system("python ./t/moxi_mock.py");

kill 2, -$childpid;

exit $result;
