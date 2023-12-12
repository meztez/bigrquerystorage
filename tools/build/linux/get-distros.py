#!/usr/bin/env python3

import os
import json
import argparse

parser = argparse.ArgumentParser(description="Print distros as JSON.")
parser.add_argument(
    'distros',
    type = str,
    nargs = '?',
    default = 'all',
    help = 'Comma-separated list of distros. Default is "all" to use all distros.'
)
args = parser.parse_args()

conts = os.listdir("tools/build/linux")
dfs = [ x for x in conts if x.startswith("Dockerfile-") ]
distros = [ x.replace("Dockerfile-", "") for x in dfs ]

if args.distros != 'all' and args.distros != '':
    distros2 = args.distros.split(",")
    distros2 = [ x.strip() for x in distros2 ]
    distros = [ x for x in distros2 if x in distros ]

print(json.dumps(distros))
