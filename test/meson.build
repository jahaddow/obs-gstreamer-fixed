#
# obs-gstreamer. OBS Studio plugin.
# Copyright (C) 2018-2021 Florian Zwoch <fzwoch@gmail.com>
#
# This file is part of obs-gstreamer.
#
# obs-gstreamer is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 2 of the License, or
# (at your option) any later version.
#
# obs-gstreamer is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with obs-gstreamer. If not, see <http://www.gnu.org/licenses/>.
#

test_deps = [
  dependency('glib-2.0'),
  dependency('gstreamer-1.0'),
  dependency('gstreamer-audio-1.0'),
]

m_dep = meson.get_compiler('c').find_library('m', required : false)
if m_dep.found()
  test_deps += m_dep
endif

steady_clock_test = executable('steady-clock-test',
  'steady-clock-test.c',
  '../steady-clock.c',
  dependencies : test_deps,
)

test('steady-clock', steady_clock_test,
  timeout : 10,
)
