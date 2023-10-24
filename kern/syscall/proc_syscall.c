/*
 * AUthor: G.Cabodi
 * Very simple implementation of sys__exit.
 * It just avoids crash/panic. Full process exit still TODO
 * Address space is released
 */

#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <proc.h>
#include <thread.h>
#include <addrspace.h>
#include <mips/trapframe.h>
#include <current.h>
#include <synch.h>
#include <kern/errno.h>
#include <proc.h>

void sys__exit(int status)
{
#if OPT_WAITPID
  struct proc *p = curproc;

  p->p_status = status & 0xff; /* just lower 8 bits returned */
  proc_remthread(curthread);
  V(p->p_sem);
#else
  /* get address space of current process and destroy */
  struct addrspace *as = proc_getas();
  as_destroy(as);
#endif

  #if OPT_FORK
    //rimuovi il processo dal padre
    remove_child(proc_search_pid(p->parent_pid), p->p_pid);
  #endif

  /* thread exits. proc data structure will be lost */
  thread_exit();

  panic("thread_exit returned (should not happen)\n");
}

int sys_waitpid(pid_t pid, userptr_t statusp, int options)
{
#if OPT_WAITPID
  struct proc *p = proc_search_pid(pid);
  int s;
  (void)options; /* not handled */
  if (p == NULL)
    return -1;
  s = proc_wait(p);
  if (statusp != NULL)
    *(int *)statusp = s;
  return pid;
#else
  (void)options; /* not handled */
  (void)pid;
  (void)statusp;
  return -1;
#endif
}

pid_t sys_getpid(void)
{
#if OPT_WAITPID
  KASSERT(curproc != NULL);
  return curproc->p_pid;
#else
  return -1;
#endif
}

#if OPT_FORK
static void
call_enter_forked_process(void *tfv, unsigned long dummy)
{
  struct trapframe *tf = (struct trapframe *)tfv;
  (void)dummy;
  enter_forked_process(tf);

  panic("enter_forked_process returned (should not happen)\n");
}

int sys_fork(struct trapframe *ctf, pid_t *retval)
{
  struct trapframe *tf_child;
  struct proc *newp;
  int result;

  KASSERT(curproc != NULL);

  newp = proc_create_runprogram(curproc->p_name);
  if (newp == NULL)
  {
    return ENOMEM;
  }
  
  //duplichiamo l'address space del nuovo processo 
  as_copy(curproc->p_addrspace, &(newp->p_addrspace));
  if (newp->p_addrspace == NULL)
  {
    proc_destroy(newp);
    return ENOMEM;
  }

  //necessitiamo di una copa del traframe del parent
  tf_child = kmalloc(sizeof(struct trapframe));
  if (tf_child == NULL)
  {
    proc_destroy(newp);
    return ENOMEM;
  }
  memcpy(tf_child, ctf, sizeof(struct trapframe));

  //copiamo la open file table del processo padre nel figlio
  copyOpenFileTable(curproc, newp);

  add_child(curproc, newp);
  newp->parent_pid = curproc->p_pid;

  result = thread_fork(
      curthread->t_name, newp,
      call_enter_forked_process, //call_enter_forked_process create an user process
      (void *)tf_child, (unsigned long)0 /*unused*/);

  if (result){
    proc_destroy(newp);
    kfree(tf_child);
    return ENOMEM;
  }

  *retval = newp->p_pid;  //to the parent we return new process id

  return 0;
}
#endif

#if OPT_SHELL
int sys_execv(const char *program, char * args[]){
  KASSERT(curproc != NULL);
  int err;

  if(program == NULL || strcmp(program, "") == 0){
    return ENOENT;
  }

  //spostamento dei dati da user a kernel
  //esplicitiamo che facciamo riferimento ad aree utente
  userptr_t uprogram = (userptr_t) program;
  userptr_t uargv = (userptr_t)args;
	struct arg_buf kargv;

  //spostamento program da user a kernel
  char *kpath = (char *) kmalloc(PATH_MAX * sizeof(char));
  if (kpath == NULL) {
		return ENOMEM;
	}
  err = copyinstr(uprogram, kpath, PATH_MAX, NULL);
  if(err) {kfree(kpath); return ENOMEM;}

  //spostamento args da memoria user a a memoria kernel

  //prima di fare ciò andiamo a calcolare il numero di argomenti e la dimensione totale
  int num_args = 0;
  int len_args = 0;
  while(args[num_args] != NULL){ //args è un vettore di stringa --> ogni stringa avrà il suo \0
    char * s = args[num_args];
    num_args++;
    len_args += strlen(s) + 1; //+1 ==> \0
  }

	argbuf_init(&kargv);
	err = argbuf_fromuser(&kargv, uargv, num_args, len_args);
  if(err) { 
    kfree(kargv.data);
    kfree(kpath);
    return ENOMEM;
  }

  //carica l'eseguibile nello stack 
  vaddr_t entrypoint, stackptr;
  load_program(kpath, &entrypoint, &stackptr);

  //andiamo a passare i puntatori allo stack e a dove andiamo a memorizzare i dati
  err = argbuf_touser(&kargv, &stackptr, &uargv);
  if (err) {
		panic("execv: copyout_args failed: %s\n", strerror(err));
	}
  //non servono più
  kfree(kargv.data);
  kfree(kpath);

  enter_new_process(kargv.nargs, uargv, NULL /*uenv*/, stackptr, entrypoint);

  panic("enter_new_process returned\n");
	return EINVAL;
}
#endif