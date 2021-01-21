#!/usr/bin/env python3

# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import argparse
import json
import os
import requests
import shutil
import subprocess
import urllib.parse
from wand.image import Image

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description = 'Bundle Goupile instance as EXE for offline use')
    parser.add_argument('url', type = str, help = 'URL of instance')
    parser.add_argument('-O', '--output_file', dest = 'output', action = 'store', help = 'Output file')
    args = parser.parse_args()

    # Find repository directory
    root_directory = os.path.dirname(os.path.realpath(__file__))
    while not os.path.exists(root_directory + '/FelixBuild.ini'):
        new_directory = os.path.realpath(root_directory + '/..')
        if new_directory == root_directory:
            raise ValueError('Could not find FelixBuild.ini')
        root_directory = new_directory

    # Download manifest information
    url = args.url.rstrip('/') + '/manifest.json'
    response = requests.get(url)
    if response.status_code != 200:
        raise ValueError('Failed to download manifest.json from this instance')
    manifest = json.loads(response.content)

    # Prepare build directory
    build_directory = os.path.join(root_directory, 'bin/GoupilePortable', manifest["name"])
    os.makedirs(build_directory + '/build', exist_ok = True)
    shutil.copytree('app', build_directory, dirs_exist_ok = True)

    # Update package.json
    with open(build_directory + '/package.json', 'r') as f:
        package = json.load(f)
    package["name"] = manifest["name"]
    package["homepage"] = args.url
    with open(build_directory + '/package.json', 'w') as f:
        json.dump(package, f, indent = 4, ensure_ascii = False)

    # Prepare PNG and icon
    url = args.url.rstrip('/') + '/favicon.png'
    response = requests.get(url)
    if response.status_code != 200:
        raise ValueError('Failed to download favicon.png from this instance')
    with open(build_directory + '/build/icon.png', 'wb') as f:
        f.write(response.content)
    with Image() as ico:
        with Image(filename = build_directory + '/build/icon.png') as img:
            for size in [16, 24, 32, 48, 64, 96, 128, 256]:
                with img.clone() as it:
                    it.resize(size, size)
                    ico.sequence.append(it)
        ico.save(filename = build_directory + '/build/icon.ico')

    # Run electron-builder
    subprocess.run('npm install', shell = True, cwd = build_directory)
    subprocess.run('npm run dist', shell = True, cwd = build_directory)

    # Copy artifact file to final destination
    output = args.output
    if output is None:
        output = f'{build_directory}/../{manifest["name"]}Portable.exe'
    shutil.copy(f'{build_directory}/dist/{manifest["name"]}.exe', output)