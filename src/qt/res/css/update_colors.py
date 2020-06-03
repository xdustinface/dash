#!/usr/bin/python3
#
# update_colors.py updates the color files in css/colors and the `<colors></colors>`
# section in all css files.
# file.
#
# Copyright (c) 2020 The Dash Core developers
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import os
from pathlib import Path
import re
import subprocess
import sys

USAGE = 'USAGE: ' + Path(__file__).name + '\n\nPARAMETER:\n\n'

MATCH_REPLACE = '<colors>.+?</colors>'
MATCH_COLORS = '#(?:[0-9a-f]{2}){2,4}|#(?:[0-9a-f]{1}){3}'

def error(msg):
    exit('\nERROR: ' + msg + "\n\n" + USAGE)

def parse_css(file_css):
    # Temporarily
    state = 0
    selectors = []

    # Results
    by_attribute = {}
    by_color = {}

    for line in file_css.read_text().splitlines():

        if line == '':
            continue

        # start of a comment
        if state == 0 and line.startswith('/*'):
            if '*/' in line:
                state = 0
            else:
                state = 1
        # we are in a comment section
        elif state == 1:
            # end of the comment
            if '*/' in line:
                state = 0
            else:
                continue
        # first line of multiple selector
        elif (state == 0 or state == 2) and ',' in line:
            state = 2
        # first line of single selector or end of multiple
        elif (state == 0 or state == 2) and '{' in line:
            state = 3
        # end of element
        elif state == 4 and line == '}':
            state = 0

        if state == 0 and len(selectors):
            selectors = []

        if state == 2:
            selector = line.split(",")[0].strip(' ')
            selectors.append(selector)

        if state == 3:
            selector = line.split("{")[0].strip(' ')
            selectors.append(selector)
            state = 4
            continue

        if state == 4:
            matched_colors = re.findall(MATCH_COLORS, line)

            if len(matched_colors) > 1:
                error("Multiple colors in a line.\n\n  {}\n\nSeems to be an invalid file!".format(line))
            elif len(matched_colors) == 1:
                matched_color = matched_colors[0]
                element = line.split(":")[0].strip(' ')

                if not matched_color in by_color:
                    by_color[matched_color] = []

                by_color[matched_color].append(element)

                entry = element + " " + matched_color

                if not entry in by_attribute:
                    by_attribute[entry] = []

                by_attribute[entry].extend(selectors)

    def sort_color(color):
        tmp = color[0].replace('#', '0x')
        return int(tmp, 0)

    def remove_duplicates(l):
        no_duplicates = []
        [no_duplicates.append(i) for i in l if not no_duplicates.count(i)]
        return no_duplicates

    colors = []

    # sort colors just by hex value
    if len(by_color):
        colors = sorted(by_color.items(), key=lambda x: sort_color(x))

    for k, l in by_attribute.items():
        by_attribute[k] = remove_duplicates(l)

    for k, l in by_color.items():
        by_color[k] = remove_duplicates(l)

    return {'fileName': file_css.stem, 'byAttribute': by_attribute, 'byColor': by_color, 'colors': colors}


def create_color_file(content, commit):

    str_result = "Color analyse of " +\
                 content['fileName'] + ".css " + \
                 "by " + \
                 Path(__file__).name + \
                 " for commit " + \
                 commit + \
                 "\n\n"

    if not len(content['colors']):
        return None

    str_result += "# Used colors\n\n"
    for c in content['colors']:
        str_result += c[0] + '\n'

    str_result += "\n# Grouped by attribute\n"

    for k, v in content['byAttribute'].items():
        str_result += '\n' + k + '\n'
        for val in v:
            str_result += '  ' + val + '\n'

    str_result += "\n# Grouped by color\n"

    for k, v in content['byColor'].items():
        str_result += '\n' + k + '\n'
        for val in v:
            str_result += '  ' + val + '\n'

    return str_result

def run(path, create_color_files=True):

    p = Path(path)

    if not p.exists():
        error("Path doesn't exist: {}".format(Path(path).absolute()))

    if not len(list(p.glob('*.css'))):
        error("No .css files found in {}".format(Path(path).absolute()))

    parsed = [parse_css(x) for x in p.glob('*.css') if x.is_file()]
    commit = subprocess.check_output(['git', '-C', p.absolute(), 'rev-parse', '--short', 'HEAD']).decode("utf-8")

    if create_color_files:

        for f in parsed:

            str_result = create_color_file(f, commit)

            if str_result is not None:
                p = Path(__file__).parent / Path('colors/' + f['fileName'] + '_css_colors.txt')
                p.write_text(str_result)

                print('\n{}.css -> {} created!'.format(f['fileName'], p.name))
            else:
                print('\n{}.css -> No colors found..'.format(f['fileName'] + ".css"))

    return parsed


if __name__ == '__main__':

    if len(sys.argv) > 1:
        error('No argument required!')

    path = str(Path(__file__).parent.absolute())
    commit = subprocess.check_output(['git', '-C', path, 'rev-parse', '--short', 'HEAD']).decode("utf-8")
    result = run(path)

    for r in result:

        file_path = path + '/' + r['fileName'] + '.css'
        css_content = Path(file_path).read_text()
        to_replace = re.findall(MATCH_REPLACE, css_content, re.DOTALL)

        str_result = "\n# Used colors in {}.css for commit {}\n".format(r['fileName'], commit)
        for c in r['colors']:
            str_result += c[0] + '\n'

        str_replace = "<colors>\n{}\n</colors>".format(str_result)

        css_content = css_content.replace(to_replace[0], str_replace)

        css_file = Path(r['fileName'] + '.css')
        css_file.write_text(css_content)
