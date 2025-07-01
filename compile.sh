if [ "$1" = "clean" ] ; then
	cd telebot
	make clean
	cd ..
	rm -rf sinaibot telebot/CMakeCache.txt telebot/CMakeFiles
else
	cd telebot
	cmake .
	make -j
	cd ..
	cc sinaibot.c -Wall -g -Itelebot/include -Ltelebot -ltelebot \
		-Wl,-rpath=$(pwd)/telebot -o ./sinaibot
fi
