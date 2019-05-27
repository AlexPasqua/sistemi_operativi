#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <time.h>
#include <sys/shm.h>
#include <sys/wait.h>

#include "errExit.h"
#include "myfifo.h"
#include "semaphores.h"
#include "sha_mem.h"

#define THOUSAND_BILLIONS 1000000000000

// variabili globali
pid_t km_pid;
int fifoserver, semid, shmid;
char *fifoserv_pathname;
struct Entry *shmptr;
/*sono glibali perché servono in "close_all()", ma non posso passarle per
argomento perché la funzione è chiamata da un signal handler che per costruzione
ha un solo argomento (il signal)*/


// funz per le operazioni pre-chiusura del processo
void close_all(){
  // aspetto la terminazione di KeyManager per non farlo diverntare orfano
  if (waitpid(km_pid, NULL, 0) == -1)
    errExit("Server: waitpid() failed");

  // chiudo ed elimino FIFOSERVER
  if (close(fifoserver) == -1)
    errExit("Server failed to close FIFOSERVER");

  if (unlink(fifoserv_pathname) != 0)
    errExit("Server failed to unlink FIFOSERVER");

  // elimino il set di semafori per le FIFO
  if (semctl(semid, 0/*ignored*/, IPC_RMID, NULL) == -1)
    errExit("Server failed to remove semaphores set");

  // detach & delete memoria condivisa
  if (shmdt(shmptr) != 0)
    errExit("Server: shmdt failed");

  if (shmctl(shmid, IPC_RMID, NULL) != 0)
    errExit("Server failed to delete shared memory segment");
}

//------------------------------------------------------------------------------
int main (int argc, char *argv[]) {
  printf("Server ready!\n\n");

  // blocco tutti i signal tranne SIGTERM
  sigset_t signal_set;
  if (sigfillset(&signal_set) == -1)
    errExit("sigfillset failed");

  if (sigdelset(&signal_set, SIGTERM) == -1)
    errExit("sigdelset failed");

  if (sigprocmask(SIG_SETMASK, &signal_set, NULL) == -1)
    errExit("sigprocmask failed");


  // creo il segmento di memoria condivisa
  key_t shm_key = ftok("src/server.c", 'a');
  if (shm_key == -1)
    errExit("Server failed to create a key for the shared mem segment");

  int shmid = shmget(shm_key, SHMDIM * sizeof(struct Entry), IPC_CREAT | S_IRUSR | S_IWUSR);
  if (shmid == -1)
    errExit("Server: shmget failed");





  // creo KeyManager
  pid_t km_pid = fork();
  if (km_pid == -1)
    errExit("Server: fork() failed");

  else if (km_pid == 0){
    //----KEY MANAGER SECTION

    // "attach" della memoria condivisa
    struct Entry *shmptr = (struct Entry *) shmat(shmid, NULL, 0);
    if (shmptr == (void *)(-1))
      errExit("Server: shmat failed");

  }
  else{
    //----PARENT SECTION

    // creo un insieme di semafori per gestire la comunicaz su FIFO
    key_t sem_key = ftok("src/semaphores.c", 'a');
    if (sem_key == -1)
      errExit("Server failed to create a key for the semaphores set");

    int semid = semget(sem_key, 2, IPC_CREAT | S_IRUSR | S_IWUSR);
    if (semid == -1)
      errExit("Server failed to perform semget");

    unsigned short sem_values[2] = {0, 1}; /* il primo è per il server,
    il secondo è per mutua esclusione tra i client*/
    union semun arg;
    arg.array = sem_values;
    if (semctl(semid, 0/*ignored*/, SETALL, arg) == -1)
      errExit("Server failed to set semaphores values");


    // creo e apro FIFOSERVER
    char *fifoserv_pathname = "/tmp/FIFOSERVER";
    char *fifocli_pathname = "/tmp/FIFOCLIENT";
    if (mkfifo(fifoserv_pathname, S_IRUSR | S_IWUSR) == -1)
      errExit("mkfifo (FIFOSERVER) failed");

    int fifoserver = open(fifoserv_pathname, O_RDONLY);
    if (fifoserver == -1)
      errExit("Server failed to open FIFOSERVER in read-only mode");


    // "attach" della memoria condivisa
    struct Entry *shmptr = (struct Entry *) shmat(shmid, NULL, 0);
    if (shmptr == (void *)(-1))
      errExit("Server: shmat failed");



    // continua a controllare richieste dei client
    struct Request client_data;
    struct Response resp;
    int bR, entry_idx = 0;
    while (1){
      // blocco il server finché un client non crea FIFOCLIENT
      semOp(semid, SRVSEM, -1);

      // apro FIFOCLIENT
      int fifoclient = open(fifocli_pathname, O_WRONLY);
      if (fifoclient == -1)
        errExit("Server failed to open FIFOCLIENT in write-only mode");

      // leggo i dati dal server
      bR = read(fifoserver, &client_data, sizeof(struct Request));
      if (bR == -1) { errExit("Server failed to perdorm a read from FIFOSERVER"); }
      else if (bR != sizeof(struct Request)) { errExit("Looks like server didn't received a struct Request correctly"); }


      printf("%s - %s, sto generando una chiave di utilizzo...\n", client_data.user, client_data.service);

      /* genero la chiave:
       *  prendo il timestamp, accodo il numero corrispondente all'iniziale
       *  dello user, una cifra per il servizio (0=stampa, 1=salva, 2=invia)
       *  e una cifra casuale. Dopodiché elimino le prime 3 cifre (che sono sempre uguali)
       *
       *  per il servizio controllo solo i primi 2 caratteri di service
       *  (sono già sicuro che le stringhe siano corrette)
       */
      unsigned long timestamp = time(NULL);
      srand(timestamp);
      resp.key = ((timestamp * 100000) + (client_data.user[0] * 100) +
                 ((client_data.service[0] == 'i') ? 20 : ((client_data.service[1] == 't') ? 0 : 10)) +
                 (rand() % 10)) % THOUSAND_BILLIONS;

      //TO_DO->scrivi su memoria condivisa (occhio ai semafori)

      // rispondo al client
      if (write(fifoclient, &resp, sizeof(struct Response)) != sizeof(struct Response))
        errExit("Server failed to write on FIFOCLIENT");

      printf("KEY = %lu\n\n", resp.key);

      // chiudo FIFOCLIENT
      if (close(fifoclient) == -1)
        errExit("Server failed to close FIFOCLIENT");

      // sblocco un client in attesa
      semOp(semid, CLIMUTEX, 1);
    }

    close_all();
  }


  return 0;
}
