#!/usr/bin/perl -w
use strict;
use Fcntl;
use IO::Socket;

if (scalar @ARGV != 3) {
   print "usage: $0 <packet_file> <host> <port> [#_of_times_to_send] [delay]\n";
   exit -1;
}

my ($file, $host, $port, $num, $delay) = @ARGV;

$num = 1 unless defined $num;
$delay = 1 unless defined $delay;

my $pkt;
sysopen (PKT, $file, O_RDONLY) || die "can't open() $file: $!";
binmode PKT;
sysread (PKT, $pkt, -s $file);
close PKT;

for (my $i = 0; $i < $num; $i++) {
   my $sock = new IO::Socket::INET (
      Proto	=> 'tcp',
      PeerAddr	=> $host,
      PeerPort	=> $port,
   );

   unless ($sock) {   
      warn "can't connect() to $host:port";
      next;
   }

   binmode $sock;
   $sock->autoflush (1);

   syswrite ($sock, $pkt);
   print while <$sock>;
   shutdown ($sock, 2);
   close $sock;

   sleep $delay;
}

exit;
