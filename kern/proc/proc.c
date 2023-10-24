/*
 * Copyright (c) 2013
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Process support.
 *
 * There is (intentionally) not much here; you will need to add stuff
 * and maybe change around what's already present.
 *
 * p_lock is intended to be held when manipulating the pointers in the
 * proc structure, not while doing any significant work with the
 * things they point to. Rearrange this (and/or change it to be a
 * regular lock) as needed.
 *
 * Unless you're implementing multithreaded user processes, the only
 * process that will have more than one thread is the kernel process.
 */

#include <types.h>
#include <spl.h>
#include <proc.h>
#include <current.h>
#include <addrspace.h>
#include <vnode.h>
#include "limits.h"
#include <kern/fcntl.h>
#include <copyinout.h>


#if OPT_WAITPID
#include <synch.h>

// tabella gestione processo
// associazione PID -> processo
#define MAX_PROC 100
static struct _processTable
{
	int active;						 /* initial value 0 */
	struct proc *proc[MAX_PROC + 1]; /* [0] not used. pids are >= 1 */
	int last_i;						 /* index of last allocated pid */
	struct spinlock lk;				 /* Lock for this table */
} processTable;
#endif

/*
 * The process for the kernel; this holds all the kernel-only threads.
 */
struct proc *kproc;

// nuova funzione per cercare un processo
struct proc *
proc_search_pid(pid_t pid)
{
#if OPT_WAITPID
	struct proc *p;
	KASSERT(pid >= 0 && pid <= MAX_PROC);
	p = processTable.proc[pid];
	KASSERT(p->p_pid == pid);
	return p;
#else
	(void)pid;
	return NULL;
#endif
}

// funzione statica per inizializzare il waitpid nella tabella
static void
proc_init_waitpid(struct proc *proc, const char *name)
{
#if OPT_WAITPID
	/* search a free index in table using a circular strategy */
	int i;
	spinlock_acquire(&processTable.lk);
	i = processTable.last_i + 1;
	proc->p_pid = 0;
	if (i > MAX_PROC)
		i = 1;
	while (i != processTable.last_i)
	{
		if (processTable.proc[i] == NULL)
		{
			processTable.proc[i] = proc;
			processTable.last_i = i;
			proc->p_pid = i;
			break;
		}
		i++;
		if (i > MAX_PROC)
			i = 1;
	}
	spinlock_release(&processTable.lk);
	if (proc->p_pid == 0)
	{
		panic("too many processes. proc table is full\n");
	}
	proc->p_status = 0;
	proc->p_sem = sem_create(name, 0);
#else
	(void)proc;
	(void)name;
#endif
}

// ci inserisce gli stdin, stdout, stderr come se fossero dei file descriptor
static int insert_standard(const char * path, int fd, int openflag, struct proc * p)
{
	int result;
	struct vnode *v;
	struct systemFileTable *st = NULL;

	char * console = kstrdup("con:");

	result = vfs_open(console, openflag, 0644, &v);

	kfree(console);

	if (result) return -1;
	
	// cerchiamo in systemFileTable
	// va ad inserire il vnode nella systable alla prima posizione libera
	for (int i = 0; i < SYSTEM_OPEN_MAX; i++){
		if (sysTable[i].vn == NULL){
			sysTable[i].vn = v;
			st = &sysTable[i];
			break;
		}
	}

	if (st == NULL)
	{
		vfs_close(v);
		return -1;
	}
	p->openFileTable[fd] = st; // openFileTable è un puntatore alla SystemFileTable
	st->offset = 0;
	st->mode_open = openflag;
	st->count_refs = 1;
	st->lock = lock_create(path);
	return 0;
}

static void
proc_end_waitpid(struct proc *proc)
{
#if OPT_WAITPID
	/* remove the process from the table */
	int i;
	spinlock_acquire(&processTable.lk);
	i = proc->p_pid;
	KASSERT(i > 0 && i <= MAX_PROC);
	processTable.proc[i] = NULL;
	spinlock_release(&processTable.lk);

	sem_destroy(proc->p_sem);
#else
	(void)proc;
#endif
}
/*
 * Create a proc structure.
 */
static struct proc *
proc_create(const char *name)
{
	struct proc *proc;

	proc = kmalloc(sizeof(*proc));
	if (proc == NULL)
	{
		return NULL;
	}
	proc->p_name = kstrdup(name);
	if (proc->p_name == NULL)
	{
		kfree(proc);
		return NULL;
	}

	proc->p_numthreads = 0;
	spinlock_init(&proc->p_lock);

	/* VM fields */
	proc->p_addrspace = NULL;

	/* VFS fields */
	proc->p_cwd = NULL;

	// alla creazione del processo inizializzo waitpid
	proc_init_waitpid(proc, name);

#if OPT_FILESYSTEM
	bzero(proc->openFileTable, OPEN_MAX * sizeof(struct systemFileTable *));
#endif

#if OPT_FORK
	proc->parent_pid = 1; // os161 manual
	proc->child = NULL;	  // lista child vuota
#endif

	return proc;
}


/*
 * Destroy a proc structure.
 *
 * Note: nothing currently calls this. Your wait/exit code will
 * probably want to do so.
 */
void proc_destroy(struct proc *proc)
{
	/*
	 * You probably want to destroy and null out much of the
	 * process (particularly the address space) at exit time if
	 * your wait/exit design calls for the process structure to
	 * hang around beyond process exit. Some wait/exit designs
	 * do, some don't.
	 */

	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/*
	 * We don't take p_lock in here because we must have the only
	 * reference to this structure. (Otherwise it would be
	 * incorrect to destroy it.)
	 */

	/* VFS fields */
	if (proc->p_cwd)
	{
		VOP_DECREF(proc->p_cwd);
		proc->p_cwd = NULL;
	}

	/* VM fields */
	if (proc->p_addrspace)
	{
		/*
		 * If p is the current process, remove it safely from
		 * p_addrspace before destroying it. This makes sure
		 * we don't try to activate the address space while
		 * it's being destroyed.
		 *
		 * Also explicitly deactivate, because setting the
		 * address space to NULL won't necessarily do that.
		 *
		 * (When the address space is NULL, it means the
		 * process is kernel-only; in that case it is normally
		 * ok if the MMU and MMU- related data structures
		 * still refer to the address space of the last
		 * process that had one. Then you save work if that
		 * process is the next one to run, which isn't
		 * uncommon. However, here we're going to destroy the
		 * address space, so we need to make sure that nothing
		 * in the VM system still refers to it.)
		 *
		 * The call to as_deactivate() must come after we
		 * clear the address space, or a timer interrupt might
		 * reactivate the old address space again behind our
		 * back.
		 *
		 * If p is not the current process, still remove it
		 * from p_addrspace before destroying it as a
		 * precaution. Note that if p is not the current
		 * process, in order to be here p must either have
		 * never run (e.g. cleaning up after fork failed) or
		 * have finished running and exited. It is quite
		 * incorrect to destroy the proc structure of some
		 * random other process while it's still running...
		 */
		struct addrspace *as;

		if (proc == curproc)
		{
			as = proc_setas(NULL);
			as_deactivate();
		}
		else
		{
			as = proc->p_addrspace;
			proc->p_addrspace = NULL;
		}
		as_destroy(as);
	}

	KASSERT(proc->p_numthreads == 0);
	spinlock_cleanup(&proc->p_lock);

	proc_end_waitpid(proc);

// Facciamo la free della lista dei processi figli
// per i figli li assegniamo al processo con pid 1 (stile UNIX)
#if OPT_FORK
	struct lista_child * child = proc->child;
	struct lista_child * next = NULL;
	struct proc * p = NULL;

	//se padre muore --> figli orfani
	while (child != NULL)
	{
		p = proc_search_pid(child->pid);

		spinlock_acquire(&p->p_lock);
		//settiamo il parent pid a 1 che significa che sono Orfani 
		p->parent_pid = 1;
		spinlock_release(&p->p_lock);

		next = child->next;
		kfree(child);
		child = next;
	}
#endif

	kfree(proc->p_name);
	kfree(proc);
}

/*
 * Create the process structure for the kernel.
 */
void proc_bootstrap(void)
{
	kproc = proc_create("[kernel]");
	if (kproc == NULL)
	{
		panic("proc_create for kproc failed\n");
	}
#if OPT_WAITPID
	spinlock_init(&processTable.lk);
	/* kernel process is not registered in the table */
	processTable.active = 1;
#endif
}

/*
 * Create a fresh proc for use by runprogram.
 *
 * It will have no address space and will inherit the current
 * process's (that is, the kernel menu's) current directory.
 */
struct proc *
proc_create_runprogram(const char *name)
{
	struct proc *newproc;

	newproc = proc_create(name);
	if (newproc == NULL)
	{
		return NULL;
	}

	/* VM fields */

	newproc->p_addrspace = NULL;

	/* VFS fields */

	/*
	 * Lock the current process to copy its current directory.
	 * (We don't need to lock the new process, though, as we have
	 * the only reference to it.)
	 */
	spinlock_acquire(&curproc->p_lock);
	if (curproc->p_cwd != NULL)
	{
		VOP_INCREF(curproc->p_cwd);
		newproc->p_cwd = curproc->p_cwd;
	}
	spinlock_release(&curproc->p_lock);

	#if OPT_FILESYSTEM
	// creo subito i vnode 0, 1, 2 per lo stdin, stodout, stderr
	if(insert_standard("STDIN", 0, O_RDONLY, newproc) == -1) return NULL;
	if(insert_standard("STDOUT",1, O_WRONLY, newproc) == -1) return NULL;
	if(insert_standard("STDERR",2, O_WRONLY, newproc) == -1) return NULL;
	#endif

	return newproc;
}

/*
 * Add a thread to a process. Either the thread or the process might
 * or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
int proc_addthread(struct proc *proc, struct thread *t)
{
	int spl;

	KASSERT(t->t_proc == NULL);

	spinlock_acquire(&proc->p_lock);
	proc->p_numthreads++;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = proc;
	splx(spl);

	return 0;
}

/*
 * Remove a thread from its process. Either the thread or the process
 * might or might not be current.
 *
 * Turn off interrupts on the local cpu while changing t_proc, in
 * case it's current, to protect against the as_activate call in
 * the timer interrupt context switch, and any other implicit uses
 * of "curproc".
 */
void proc_remthread(struct thread *t)
{
	struct proc *proc;
	int spl;

	proc = t->t_proc;
	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	KASSERT(proc->p_numthreads > 0);
	proc->p_numthreads--;
	spinlock_release(&proc->p_lock);

	spl = splhigh();
	t->t_proc = NULL;
	splx(spl);
}

/*
 * Fetch the address space of (the current) process.
 *
 * Caution: address spaces aren't refcounted. If you implement
 * multithreaded processes, make sure to set up a refcount scheme or
 * some other method to make this safe. Otherwise the returned address
 * space might disappear under you.
 */
struct addrspace *
proc_getas(void)
{
	struct addrspace *as;
	struct proc *proc = curproc;

	if (proc == NULL)
	{
		return NULL;
	}

	spinlock_acquire(&proc->p_lock);
	as = proc->p_addrspace;
	spinlock_release(&proc->p_lock);
	return as;
}

/*
 * Change the address space of (the current) process. Return the old
 * one for later restoration or disposal.
 */
struct addrspace *
proc_setas(struct addrspace *newas)
{
	struct addrspace *oldas;
	struct proc *proc = curproc;

	KASSERT(proc != NULL);

	spinlock_acquire(&proc->p_lock);
	oldas = proc->p_addrspace;
	proc->p_addrspace = newas;
	spinlock_release(&proc->p_lock);
	return oldas;
}

int proc_wait(struct proc *proc)
{
#if OPT_WAITPID
	int return_status;
	/* NULL and kernel proc forbidden */
	KASSERT(proc != NULL);
	KASSERT(proc != kproc);

	/* wait on semaphore  */
	P(proc->p_sem);

	return_status = proc->p_status;
	proc_destroy(proc); // eliminiamo definitivamente il processo
	return return_status;
#else
	/* this doesn't synchronize */
	(void)proc;
	return 0;
#endif
}

// dobbiamo copiare la open file table del vecchio processo nel nuovo
void copyOpenFileTable(struct proc *parent, struct proc *child)
{
	for (int i = 0; i < OPEN_MAX; i++){
		child->openFileTable[i] = parent->openFileTable[i];
	}
}

#if OPT_FORK
void add_child(struct proc *parent, struct proc *child){
	struct lista_child * c = parent->child;

	spinlock_acquire(&parent->p_lock);

	if(c == NULL){
		parent->child = kmalloc(sizeof(struct lista_child)); //aggiungo il figlio alla lista
		parent->child->pid = child->p_pid; //al figlio segno il pid del padre
		
		parent->child->next = NULL;
		
	}
	else{
		while(c->next != NULL){
			c = c->next;
		}

		c->next = kmalloc(sizeof(struct lista_child)); //aggiungo il figlio alla lista
		c->next->pid = child->p_pid; //al figlio segno il pid del padre
		c->next->next = NULL;
	}

	spinlock_release(&parent->p_lock);
}


void remove_child(struct proc *parent, pid_t pid){
	struct lista_child * c = parent->child;
	struct lista_child * before = NULL;

	spinlock_acquire(&parent->p_lock);

	while(c != NULL){
		if(c->pid == pid){
			if(before == NULL)//caso primo elemento
				parent->child = c->next;
			else //collegamento
				before->next = c->next;
			
			kfree(c);
			break;
		}else{ //avanzo sia con il before che col next
			before = c;
			c = c->next;
		}
	}

	spinlock_release(&parent->p_lock);
}
#endif

///////////////////////////////////////////////
//QUA SOTTO FUNZIONI PER EXECV
//////////////////////////////////////////////
#if OPT_SHELL

//inizializzazione buffer
void argbuf_init(struct arg_buf *buf) {
	buf->data = NULL;
	buf->len = 0;
	buf->nargs = 0;
	buf->max = 0;
}

//copia da memoria utente a memoria kernel
int argbuf_fromuser(struct arg_buf *buf, userptr_t uargv, int num_args, int len_args) {
	int result;

	if(len_args > ARG_MAX){
		return E2BIG;
	}

	buf->data = (char *) kmalloc(len_args * sizeof(char));
	
	if (buf->data == NULL) {
		return ENOMEM;
	}

	buf->nargs = num_args;
	buf->max = len_args * sizeof(char);

	result = argbuf_copyin(buf, uargv);

	return result;
}

int argbuf_copyin(struct arg_buf *buf, userptr_t uargv) {
    userptr_t thisarg;
	size_t thisarglen;
	int result;

	int count_args = 0;
	while (count_args < buf->nargs) {
		//in copyin andiamo a prendere il riferiemento al argomento (dimension userptr_t)
		//il riferimento è inserito in thisarg
		result = copyin(uargv , &thisarg, sizeof(userptr_t));
		if (result) {
			return result;
		}

		//Copia l'argomento dentro il kernel , nella posizione bufdata + buf len
		//thisarglen contiene quanto ha scritto sul buffer
		result = copyinstr(thisarg, buf->data + buf->len, buf->max - buf->len, &thisarglen);
		if (result) {
			return result;
		}

		uargv += sizeof(userptr_t);
		buf->len += thisarglen;
		count_args++;
	}

	KASSERT(buf->max == buf->len); //dobbiamo aver occupato tutta l'area

	return 0;
}

//Codice simile a runprogram: creiamo un nuovo address space
int load_program(char *path, vaddr_t *entrypoint, vaddr_t *stackptr){
	struct addrspace *newas, *oldas;
	struct vnode *vn;
	char *newname;
	int err;

	
	newname = kstrdup(path);
	if (newname == NULL) {
		return ENOMEM;
	}

	err = vfs_open(path, O_RDONLY, 0, &vn);
	if (err) {
		kfree(newname);
		return err;
	}

	//creiamo un nuovo address space
	newas = as_create();
	if (newas == NULL) {
		vfs_close(vn);
		kfree(newname);
		return ENOMEM;
	}

	//proc_setas cambia l'address space del processo corrente
	//e restituire l'address space vecchio
	//==> possiamo quindi rispristinare il vecchio in caso di errori e spostare i dati
	oldas = proc_setas(newas);
	as_activate();

 	
	//carichiamo l'eseguibile, se fallisce ripristiamo il vecchio indirizzo
	err = load_elf(vn, entrypoint);
	if (err) {
		vfs_close(vn);
		proc_setas(oldas);
		as_activate();
		as_destroy(newas);
		kfree(newname);
		return err;
	}

	vfs_close(vn);

	//creazione stack
	err = as_define_stack(newas, stackptr);
	if (err) {
		proc_setas(oldas);
		as_activate();
		as_destroy(newas);
		kfree(newname);
		return err;
        }

	//vecchio addres space non ci serve più
	if (oldas) {
		as_destroy(oldas);
	}

	kfree(curthread->t_name);
	curthread->t_name = newname;

	return 0;
}

//buf contiene i dati dal kernel
//ustackp e uargv_ret servono a ritornare dove abbiamo memorizzato i dati
int argbuf_touser(struct arg_buf *buf, vaddr_t *ustackp, userptr_t *uargv_ret) {

	vaddr_t ustack;
	userptr_t ustringbase, uargvbase, uargv_i;
	userptr_t thisarg;
	size_t thisarglen;
	size_t pos;
	int result;

	ustack = *ustackp;

	//salviamo sullo stack la variabili della struct 
	ustack -= buf->len;
	ustack -= (ustack & (sizeof(void *) - 1)); //allineamento dello stackS
	ustringbase = (userptr_t)ustack; //puntatore per area degli argomenti

	ustack -= (buf->nargs + 1) * sizeof(userptr_t);
	uargvbase = (userptr_t)ustack;  //aggiorna la prima posizione libera nello stack

	//copaimo i dati di nuovo nello stack user 
	pos = 0;
	uargv_i = uargvbase;
	while (pos < buf->max) {
		//area dove andare a copiare gli argomenti parte da ustringbase
		thisarg = ustringbase + pos;

		result = copyout(&thisarg, uargv_i, sizeof(thisarg));
		if (result) {
			return result;
		}

		//copia della stringa
		result = copyoutstr(buf->data + pos, thisarg, buf->max - pos, &thisarglen);
		if (result) {
			return result;
		}

		
		pos += thisarglen;
		uargv_i += sizeof(thisarg);
	}
	
	KASSERT(pos == buf->max);

	//aggiungiamo il NULL a thisarg (nell'ultima posizione dove andiamo a copiarli mettiamo il valore NULL)
	thisarg = NULL;
	result = copyout(&thisarg, uargv_i, sizeof(userptr_t));
	if (result) {
		return result;
	}

	//gli argomenti del programma sono caricati tra uargvbase e ustack 
	*ustackp = ustack;
	*uargv_ret = uargvbase;
	return 0;
}
#endif