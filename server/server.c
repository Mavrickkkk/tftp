#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>

#define SIZE 516 // 4 octets d'en-tête + 512 octets de données
#define TIMEOUT 3 // Timeout en secondes
#define MAX_RETRIES 5 // Nombre maximum de tentatives de retransmission

void dieWithError(char *errorMessage, int sockfd) {
    if (sockfd >= 0) close(sockfd);
    perror(errorMessage);
    printf("[CLOSING] Closing the server.\n");
    exit(EXIT_FAILURE);
}

// Envoie un paquet d'erreur TFTP
void sendErrorPacket(int sockfd, struct sockaddr_in *addr, int errorCode, const char *errorMessage) {
    char errorBuffer[SIZE];
    int errorMessageLength = strlen(errorMessage) + 1;
    errorBuffer[0] = 0; // Premier octet du Opcode
    errorBuffer[1] = 5; // Deuxième octet du Opcode pour un paquet d'erreur
    errorBuffer[2] = (errorCode >> 8) & 0xFF; // Octet de poids fort du code d'erreur
    errorBuffer[3] = errorCode & 0xFF; // Octet de poids faible du code d'erreur
    strcpy(errorBuffer + 4, errorMessage); // Message d'erreur

    sendto(sockfd, errorBuffer, 4 + errorMessageLength, 0, (struct sockaddr *)addr, sizeof(struct sockaddr_in));
}

// Fonction pour recevoir les ACKs avec gestion du timeout
int receiveACK(int sockfd, int expectedBlockNumber, struct sockaddr_in *addr) {
    char ackBuffer[4];
    socklen_t addr_size = sizeof(struct sockaddr_in);
    fd_set read_fds;
    struct timeval timeout;

    FD_ZERO(&read_fds);
    FD_SET(sockfd, &read_fds);

    timeout.tv_sec = TIMEOUT;
    timeout.tv_usec = 0;

    int retval = select(sockfd + 1, &read_fds, NULL, NULL, &timeout);
    if (retval == -1) {
        dieWithError("[ERROR] select() failed", sockfd);
    } else if (retval == 0) {
        printf("[ERROR] Timeout occurred while waiting for ACK.\n");
        return -1; // Indique un timeout pour permettre une retransmission
    }

    // Attendre la réception d'un ACK
    int n = recvfrom(sockfd, ackBuffer, sizeof(ackBuffer), 0, (struct sockaddr *)addr, &addr_size);
    if (n < 0)
        dieWithError("[ERROR] receiving ACK", sockfd);

    // Vérifier si c'est bien un paquet ACK
    if (ackBuffer[1] != 4) {
        dieWithError("[ERROR] Not an ACK packet", sockfd);
    }

    // Extraire le numéro de bloc de l'ACK
    int blockNumber = (unsigned char)ackBuffer[2] << 8 | (unsigned char)ackBuffer[3];

    // Vérifier si le numéro de bloc est celui attendu
    if (blockNumber != expectedBlockNumber) {
        dieWithError("[ERROR] Unexpected block number in ACK", sockfd);
    }

    printf("[INFO] Received ACK for block %d\n", blockNumber);
    return 0; // ACK reçu et vérifié avec succès
}

void handle(int sockfd, struct sockaddr_in addr) {
    char buffer[SIZE];
    FILE *fp = NULL;
    socklen_t addr_size = sizeof(struct sockaddr_in);
    int n, readBytes, blockNumber, retries, ackReceived, running = 1;
    char ackPacket[4] = {0, 4, 0, 0}; // Initialisation du paquet ACK
    char *filename;

    while (running) {
        addr_size = sizeof(addr);
        n = recvfrom(sockfd, buffer, SIZE, 0, (struct sockaddr *)&addr, &addr_size);
        if (n < 0) dieWithError("[ERROR] recvfrom error", sockfd);

        switch (buffer[1]) {
            case 2: { // WRQ
                filename = buffer + 2; // Extraire le nom du fichier
                fp = fopen(filename, "w");
                if (fp == NULL) {
                    // Envoyer un paquet d'erreur indiquant que le fichier ne peut pas être créé
                    sendErrorPacket(sockfd, &addr, 2, "Cannot create file.");
                    printf("[CLOSING] Closing the server.\n");
                    if (sockfd >= 0) close(sockfd);
                    exit(EXIT_FAILURE);
                }
                // Envoi d'un premier ACK avec le numéro de bloc à 0 pour confirmer la réception de WRQ
                if (sendto(sockfd, ackPacket, sizeof(ackPacket), 0, (struct sockaddr *)&addr, addr_size) < 0)
                    dieWithError("[ERROR] sendto error", sockfd);
                break;
            }

            case 3: { // DATA
                // S'assurer qu'un fichier est ouvert pour l'écriture (WRQ reçu précédemment)
                if (fp == NULL) {
                    sendErrorPacket(sockfd, &addr, 5, "No write request received.");
                    printf("[CLOSING] Closing the server.\n");
                    if (sockfd >= 0) close(sockfd);
                    exit(EXIT_FAILURE);
                }
                // Écriture des données dans le fichier
                if (fwrite(buffer + 4, 1, n - 4, fp) < n - 4)
                    dieWithError("[ERROR] fwrite error", sockfd);
                // Préparation et envoi d'un ACK pour le bloc reçu
                ackPacket[2] = buffer[2]; // Copier l'octet de poids fort du numéro de bloc
                ackPacket[3] = buffer[3]; // Copier l'octet de poids faible du numéro de bloc
                if (sendto(sockfd, ackPacket, sizeof(ackPacket), 0, (struct sockaddr *)&addr, addr_size) < 0)
                    dieWithError("[ERROR] sendto error", sockfd);
                // Fermeture du fichier si c'est le dernier bloc
                if (n < 512) {
                    printf("[SUCCESS] File received successfully.\n");
                    fclose(fp);
                    running = 0;
                    break;
                }
                break;
            }

            case 1: { // RRQ
                // Extraire le nom du fichier
                filename = buffer + 2;
                fp = fopen(filename, "r");
                if (fp == NULL) {
                    sendErrorPacket(sockfd, &addr, 1, "File not found.");
                    printf("[CLOSING] Closing the server.\n");
                    if (sockfd >= 0) close(sockfd);
                    exit(EXIT_FAILURE);
                }
                // Envoyer le fichier en blocs de 512 octets
                blockNumber = 1;
                do {
                    memset(buffer, 0, SIZE);
                    readBytes = fread(buffer + 4, 1, 512, fp);
                    buffer[1] = 3; // Opcode pour DATA
                    // Définir le numéro de bloc
                    buffer[2] = (blockNumber >> 8) & 0xFF; // Octet de poids fort du numéro de bloc
                    buffer[3] = blockNumber & 0xFF;        // Octet de poids faible du numéro de bloc
                    retries = 0;
                    ackReceived = 0;
                    // Envoyer le bloc
                    while (retries < MAX_RETRIES && !ackReceived) {
                        printf("[SENDING] Block %d, Attempt %d\n", blockNumber, retries + 1);
                        if (sendto(sockfd, buffer, readBytes + 4, 0, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                            fclose(fp);
                            dieWithError("[ERROR] sending block to the client.", sockfd);
                        }
                        // Attendre l'ACK pour ce bloc
                        if (receiveACK(sockfd, blockNumber, &addr) == 0) {
                            ackReceived = 1; // ACK correctement reçu
                        } else {
                            printf("[RETRY] No ACK for block %d, retrying...\n", blockNumber);
                            retries++;
                        }
                    }

                    if (!ackReceived) {
                        fclose(fp);
                        dieWithError("[ERROR] Max retries reached, giving up on transfer.", sockfd);
                    }

                    blockNumber++;
                } while (readBytes == 512);

                printf("[SUCCESS] File sent successfully.\n");
                fclose(fp);
                running = 0;
                break;
            }

            default:
                sendErrorPacket(sockfd, &addr, 4, "Illegal TFTP operation.");
                running = 0;
                break;
        }

    }
}

int main() {
    // Définition de l'adresse IP et du port du serveur
    char *ip = "127.0.0.1";
    int port = 8069;

    // Création d'un socket UDP pour le serveur
    int server_sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server_sockfd < 0) {
        perror("[ERROR] socket error");
        exit(EXIT_FAILURE);
    }

    // Configuration de l'adresse du serveur
    struct sockaddr_in server_addr, client_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(ip);

    // Liaison du socket avec l'adresse du serveur
    if (bind(server_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
        dieWithError("[ERROR] bind error", server_sockfd);

    printf("[STARTING] UDP File Server started on %s:%d.\n", ip, port);
    // Gestion des demandes de clients
    handle(server_sockfd, client_addr);
    printf("[CLOSING] Closing the server.\n");

    // Fermeture du socket du serveur
    close(server_sockfd);

    return 0;
}

