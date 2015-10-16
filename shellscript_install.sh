#!/bin/bash
# Autoren: Natalia Duske, Melanie Remmels
# 17.12.2012
#------------------------------------------------------------

module=$1
minor_morse=0
minor_esrom=1

#Sicherstellen, dass die Ausführung als root erfolgt
if [ "$(whoami)" != "root" ]
  then
    echo "Please execute as root"
    exit -1
fi

if [ "$1" == "" ]
 then
  echo "No module name given"
  exit -2
fi

# Alte morse und esrom Devices entfernen
rm /dev/morse >/dev/null
rm /dev/esrom >/dev/null

# altes Kernel-Modul entfernen
/sbin/rmmod ${module}.ko

# Zusaetzliche Option -r: Nach dem Entladen abbrechen, nicht neu laden
if [ "${2}" == "-r" ]
  then
  echo "Modul ${module} wurde entladen."
  exit 0
fi

# Modul im Kernel laden
insmod ${module}.ko nopen=2 lbuf=100
if [ "$?" != "0" ]
then
    echo "Module couldnt be loaded, exiting"
    exit -3
fi 

# Die major Number suchen
# cat /proc/devices        - gibt die Devices aus
# | grep ${module}         - sucht aus der Ausgabe von cat die Zeilen heraus, in denen der Modulname vorkommt
# | tr -d "\n"             - loescht Zeilenumbrueche der Ausgabe, falls mehr als eine zeile mit dem Modulnamen vorkommt 
#                            (nur zur Sicherheit, dass kein Muell raus kommt)
# | cut -d " " -f ${field} - gibt den Eintrag an Stelle "field" aus. Nach jedem Leerzeichen beginnt ein neues Feld.
#                            Es wird beim ersten Feld angefangen und weiter gesucht, bis das erste nicht leere Feld gefunden ist.
#                            Das ist noetig, wenn vor der major devicenumber in der Ausgabe noch weitere Leerzeichen stehen.
major=""
declare -i field=1
while [ "${major}" == "" ]
do
  major=$(cat /proc/devices | grep ${module} | tr -d "\n" | cut -d " " -f ${field})
  field=field+1
done
echo "Device ${module} ist mit Major Devicenumber ${major} geladen."

#Device morse erstellen
mknod /dev/morse c ${major} ${minor_morse}

#Device esrom erstellen
mknod /dev/esrom c ${major} ${minor_esrom}

exit 0
