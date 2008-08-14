#!/bin/sh

#
# Downloads the contigs for a strain of e. coli from NCBI and builds a
# Bowtie index for it
#

GENOMES_MIRROR=ftp://ftp.ncbi.nlm.nih.gov/genomes

BOWTIE_BUILD_EXE=./bowtie-build
if [ ! -x "$BOWTIE_BUILD_EXE" ] ; then
	if ! which bowtie-build ; then
		echo "Could not find bowtie-build in current directory or in PATH"
		exit 1
	else
		BOWTIE_BUILD_EXE=`which bowtie-build`
	fi
fi

if [ ! -f NC_008253.fna ] ; then
	wget ${GENOMES_MIRROR}/Bacteria/Escherichia_coli_536/NC_008253.fna
fi

if [ ! -f NC_008253.fna ] ; then
	echo "Could not find NC_008253.fna file!"
	exit 2
fi

echo Running ${BOWTIE_BUILD_EXE} NC_008253.fna e_coli
${BOWTIE_BUILD_EXE} -t 8 NC_008253.fna e_coli