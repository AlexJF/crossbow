#!/bin/sh

##
# push.sh
#
# Run this from the $CROSSBOW_HOME/doc/website subdirectory.
#
# Copies the files that comprise the website at
# http://bowtie-bio.sourceforge.net/crossbow to sourceforge.  You must
# have the right sourceforge privileges to do this.  The SF_USER
# environment variable must be set appropriately.
#

[ -z "$SF_USER" ] && echo "Must set SF_USER" && exit 1

scp -r * $SF_USER,bowtie-bio@web.sourceforge.net:/home/groups/b/bo/bowtie-bio/htdocs/crossbow
scp -r ../images $SF_USER,bowtie-bio@web.sourceforge.net:/home/groups/b/bo/bowtie-bio/htdocs/crossbow/
