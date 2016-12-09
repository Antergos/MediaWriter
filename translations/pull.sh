#!/bin/bash

# rm -f *.qm *.po websites/getfedora/*.po websites/spins/*.po websites/labs/*.po

#transifex pull

echo -e '<RCC>' > translations.qrc
echo -e '\t<qresource prefix="/translations/">' >> translations.qrc
for pofile in *.po
do
	LANGCODE=$(basename -s .po "${pofile}")
	sed -i '/"Language:.*/a "X-Qt-Contexts: true\\n"' "${pofile}"
	lrelease-qt5 "${pofile}" -qm "${LANGCODE}.qm"
	echo -e "\t\t<file>${LANGCODE}.qm</file>" >> translations.qrc
done
echo -e '\t</qresource>' >> translations.qrc
echo -e '</RCC>' >> translations.qrc
