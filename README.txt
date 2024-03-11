Projet : Concevoir protocole de communication entre server et client

Auteurs : Thibaut FRANÇOIS 22110629
Mavrick GOUIX 22102167

Instruction de compilation :
(dans le dossier source) gcc client.c -o client
(dans le dossier server) gcc server.c -o server

2 exemples d'exécution :

Premier cas sur un serveur en local (avec server.c) :
lancer server.c avec un premier terminal : ./server
lancer client.c dans un second terminal : ./client
Enter server IP : 127.0.0.1 		//adresse du serveur en local
Enter server port : 8069 		    //port aléatoire non utilisé par aucun processus
Request (get or put) : get		    //ou put selon l'action choisie
Name of the File : filename.txt		//nom du fichier

Second cas sur un serveur existant (sans server.c) :
lancer client.c : ./client
Enter server IP : 10.1.16.112 		//adresse du serveur
Enter server port : 69 			    //port aléatoire non utilisé par aucun processus
Request (get or put) : get		    //ou put selon l'action choisie
Name of the File : filename.txt	    //nom du fichier