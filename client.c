#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define SIZE 516 // 4 octets d'en-tête + 512 octets de données
#define TIMEOUT 3 // Timeout en secondes
#define MAX_RETRIES 5 // Nombre maximum de tentatives de retransmission

void dieWithError(char *errorMessage, int sockfd, FILE *fp) {
    if (fp != NULL) fclose(fp);
    if (sockfd >= 0) close(sockfd);
    perror(errorMessage);
    exit(EXIT_FAILURE);
}

// Fonction pour gérer les erreurs TFTP
void handleTFTPError(const char *buffer, int n, int sockfd, FILE *fp) {
    if (n < 4) return; // Taille minimale d'un paquet d'erreur
    if (buffer[1] == 5) { // Opcode d'erreur est 5
        int errorCode = (unsigned char)buffer[2] << 8 | (unsigned char)buffer[3];
        printf("[ERROR] TFTP Error Code %d: %s\n", errorCode, buffer + 4);
        if (fp != NULL) fclose(fp);
        if (sockfd >= 0) close(sockfd);
        exit(EXIT_FAILURE);
    }
}

// Fonction pour recevoir les ACKs avec gestion du timeout (utilisé dans la fonction send_WRQ)
int receiveACK(int sockfd, int expectedBlockNumber, struct sockaddr_in *addr, FILE *fp) {
    char ackBuffer[4];
    socklen_t addr_size = sizeof(struct sockaddr_in);
    fd_set read_fds;
    struct timeval timeout;

    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);

    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;

    // Attendre la réception d'un ACK avec gestion du timeout
    int retval = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);
    if (retval == -1) {
        dieWithError("[ERROR] select() failed", sockfd, fp);
    } else if (retval == 0) {
        printf("[ERROR] Timeout occurred while waiting for ACK.\n");
        return -1; // Indique un timeout pour permettre une retransmission
    }

    // Attendre la réception d'un ACK
    int n = recvfrom(sockfd, ackBuffer, sizeof(ackBuffer), 0, (struct sockaddr *) addr, &addr_size);
    if (n < 0) {
        dieWithError("[ERROR] receiving ACK", sockfd, fp);
    }
    handleTFTPError(ackBuffer, n, sockfd, fp); // Cette fonction gère les paquets d'erreur

    // Vérifier si c'est bien un paquet ACK
    if (ackBuffer[1] != 4) {
        dieWithError("[ERROR] Not an ACK packet", sockfd, fp);
    }

    // Extraire le numéro de bloc de l'ACK
    int blockNumber = (unsigned char) ackBuffer[2] << 8 | (unsigned char) ackBuffer[3];

    // Vérifier si le numéro de bloc est celui attendu
    if (blockNumber != expectedBlockNumber) {
        dieWithError("[ERROR] Unexpected block number in ACK", sockfd, fp);
    }

    printf("[INFO] Received ACK for block %d\n", blockNumber);
    return 0; // ACK reçu et vérifié avec succès
}


void send_WRQ(const char *nameFile, int sockfd, struct sockaddr_in addr) {
    FILE *fp = fopen(nameFile, "rb");
    if (!fp) dieWithError("[ERROR] Could not open file for reading", sockfd, fp);

    char buffer[SIZE];
    int n, readBytes, retries, blockNumber = 0;

    // Construire et envoyer une requête WRQ
    // Nettoyage du buffer
    memset(buffer, 0, SIZE);
    // Opcode pour WRQ est 2
    buffer[1] = 2; // Le deuxième octet de l'opcode pour WRQ
    // Copier le nom du fichier et le mode dans le buffer
    strcpy(buffer + 2, nameFile);
    strcpy(buffer + 2 + strlen(nameFile) + 1, "octet");

    // Calculer la longueur du paquet WRQ
    int packetLength = 2 + strlen(nameFile) + 1 + strlen("octet") + 1;

    // Envoyer la requête WRQ
    n = sendto(sockfd, buffer, packetLength, 0, (struct sockaddr *) &addr, sizeof(addr));
    if (n < 0) {
        dieWithError("[ERROR] sending WRQ to the server.", sockfd, fp);
    }

    // Attendre et vérifier l'ACK pour la requête WRQ
    if (receiveACK(sockfd, 0, &addr, fp) != 0) {
        dieWithError("[ERROR] No ACK for WRQ or error receiving ACK.", sockfd, fp);
    }

    // Préparation du buffer pour l'envoi du fichier
    memset(buffer, 0, SIZE); // Nettoyage du buffer avant l'envoi des données

    // Envoyer le fichier par blocs de 512 octets
    while ((readBytes = fread(buffer + 4, 1, 512, fp)) > 0) {
        blockNumber++;
        retries = 0;
        while (retries < MAX_RETRIES) {
            printf("[SENDING] Block %d\n", blockNumber);
            buffer[1] = 3; // Opcode pour DATA
            // Définir le numéro de bloc
            buffer[2] = (blockNumber >> 8) & 0xFF; // Octet de poids fort du numéro de bloc
            buffer[3] = blockNumber & 0xFF;        // Octet de poids faible du numéro de bloc
            // Envoyer le bloc
            n = sendto(sockfd, buffer, readBytes + 4, 0, (struct sockaddr *) &addr, sizeof(addr));
            if (n < 0) {
                if (retries == MAX_RETRIES - 1) {
                    dieWithError("[ERROR] sending block to the server after max retries.", sockfd, fp);
                }
                printf("[RETRY] Retrying sendto...\n");
                retries++;
                continue;
            }

            // Attendre et vérifier l'ACK pour chaque bloc de données
            if (receiveACK(sockfd, blockNumber, &addr, fp) == 0) {
                break; // ACK reçu, continuer avec le prochain bloc
            } else {
                if (retries == MAX_RETRIES - 1) {
                    dieWithError("[ERROR] No ACK received for block after max retries.", sockfd, fp);
                }
                printf("[RETRY] No ACK for block, retrying...\n");
                retries++;
            }
        }
        memset(buffer, 0, SIZE);
    }

    printf("[SUCCESS] File sent successfully.\n");
    fclose(fp);
}

void send_RRQ(const char *nameFile, int sockfd, struct sockaddr_in addr) {
    FILE *fp = fopen(nameFile, "wb");
    if (!fp) dieWithError("[ERROR] Could not open file for writing", sockfd, fp);

    socklen_t addr_size = sizeof(struct sockaddr_in);
    char buffer[SIZE], ackPacket[4] = {0, 4, 0, 0};
    int n, retries, expectedBlockNumber = 1;
    struct timeval timeout;
    fd_set read_fds;

    // Construire et envoyer une requête RRQ
    // Nettoyage du buffer
    memset(buffer, 0, SIZE);
    // Opcode pour RRQ est 1
    buffer[1] = 1; // Le deuxième octet de l'opcode pour RRQ
    // Copier le nom du fichier et le mode dans le buffer
    strcpy(buffer + 2, nameFile);
    strcpy(buffer + 2 + strlen(nameFile) + 1, "octet");

    // Calculer la longueur du paquet RRQ
    int packetLength = 2 + strlen(nameFile) + 1 + strlen("octet") + 1;

    // Envoyer la requête RRQ
    if (sendto(sockfd, buffer, packetLength, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        dieWithError("[ERROR] sending RRQ to the server.", sockfd, fp);
    }

    memset(buffer, 0, SIZE);

    while (1) {
        FD_ZERO(&read_fds);
        FD_SET(sockfd, &read_fds);

        timeout.tv_sec = TIMEOUT;
        timeout.tv_usec = 0;

        retries = 0;
        while (retries < MAX_RETRIES) {
            int retval = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);
            if (retval == -1) {
                dieWithError("[ERROR] select() failed", sockfd, fp);
            } else if (retval == 0) { // Timeout
                printf("[RETRY] Timeout, retrying ACK for block %d\n", expectedBlockNumber - 1);
                // Renvoyer le dernier paquet ACK
                if (sendto(sockfd, ackPacket, sizeof(ackPacket), 0, (struct sockaddr *)&addr, addr_size) < 0) {
                    dieWithError("[ERROR] Resending ACK failed", sockfd, fp);
                }
                retries++;
                continue;
            }

            n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr *) &addr, &addr_size);
            if (n < 0) dieWithError("[ERROR] recvfrom error", sockfd, fp);
            handleTFTPError(buffer, n, sockfd, fp); // Vérifie et gère un paquet d'erreur potentiel

            if (buffer[1] == 3) { // DATA packet
                int blockNumber = (unsigned char)buffer[2] << 8 | (unsigned char)buffer[3];
                if (blockNumber == expectedBlockNumber) { // Écrire les données reçues dans le fichier
                    if (fwrite(buffer + 4, 1, n - 4, fp) < 1) {
                        dieWithError("[ERROR] fwrite error", sockfd, fp);
                    }

                    // Préparation et envoi d'un ACK pour le bloc reçu
                    ackPacket[2] = buffer[2]; // Copier l'octet de poids fort du numéro de bloc
                    ackPacket[3] = buffer[3]; // Copier l'octet de poids faible du numéro de bloc
                    if (sendto(sockfd, ackPacket, sizeof(ackPacket), 0, (struct sockaddr *) &addr, addr_size) < 0) {
                        dieWithError("[ERROR] sendto error", sockfd, fp);
                    }

                    expectedBlockNumber++;
                }

                break; // Sortie de la boucle de retransmission
            }
            retries++;
        }

        if (retries == MAX_RETRIES) {
            dieWithError("[ERROR] Max retries reached, transfer failed", sockfd, fp);
        }

        // Si c'est le dernier bloc (moins de 512 octets de données)
        if (n < 512) {
            printf("[SUCCESS] File received successfully.\n");
            fclose(fp);
            break; // Fin de la transmission
        }
    }
}


int main() {
    // Déclaration des variables
    char ip[15], nameFile[256], request[4];
    int port;

    // Requetes pour les paramètres du serveur tftp
    printf("Enter server IP : ");
    scanf("%s", ip);
    printf("Enter server port : ");
    scanf("%d", &port);
    printf("Request (get or put) : ");
    scanf("%s", request);
    printf("Name of the File : ");
    scanf("%s", nameFile);

    // Variables de définition et création d'un socket UDP
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in server_addr;
    if (sockfd < 0) {
        perror("[ERROR] socket error");
        exit(EXIT_FAILURE);
    }

    // Configuration de l'adresse du serveur
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    // Envoyer la requête WRQ ou RRQ en fonction de la demande de l'utilisateur
    if (strcmp("put", request) == 0) {
        send_WRQ(nameFile, sockfd, server_addr);
    } else if (strcmp("get", request) == 0) {
        send_RRQ(nameFile, sockfd, server_addr);
    } else {
        close(sockfd);
        printf("[ERROR] Invalid request type\n");
        exit(EXIT_FAILURE);
    }

    // Fermeture du socket
    close(sockfd);
    return 0;
}
