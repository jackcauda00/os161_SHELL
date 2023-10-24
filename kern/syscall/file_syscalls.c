/*
 * AUthor: G.Cabodi
 * Very simple implementation of sys_read and sys_write.
 * just works (partially) on stdin/stdout
 */

#include <types.h>
#include <kern/unistd.h>
#include <clock.h>
#include <copyinout.h>
#include <syscall.h>
#include <lib.h>
#include <limits.h>
#include <vfs.h>
#include <kern/errno.h>
#include <current.h>
#include <proc.h>
#include <kern/fcntl.h>
#include <kern/types.h>
#if OPT_SHELL
#include <kern/seek.h>
#include <kern/stat.h>
#endif  
#if OPT_FILESYSTEM
  #include <copyinout.h>
  #include <vnode.h>
  #include <limits.h>
  #include <uio.h>
  #include <proc.h>
  #include <synch.h>
  #include <copyinout.h>
#endif


#if OPT_FILESYSTEM
 //file read - write con buffer kernel 
 static int
file_read(int fd, userptr_t buf_ptr, size_t size, int * retval) {
  struct iovec iov;
  struct uio ku;
  int result, nread;
  struct vnode *vn;
  struct systemFileTable *sf;
  void *kbuf;
  int err;

  if(fd<0 || fd>OPEN_MAX || curproc->openFileTable[fd]==NULL){
    return EBADF;
  }

  if(buf_ptr == NULL){
    return EFAULT;
  }
  
  sf = curproc->openFileTable[fd];
  if (sf->mode_open == O_WRONLY) return EBADF;
  vn = sf->vn;
  if (vn==NULL) return -1;
  
  lock_acquire(sf->lock);

  kbuf = kmalloc(size);
  uio_kinit(&iov, &ku, kbuf, size, sf->offset, UIO_READ);
  result = VOP_READ(vn, &ku);
  
  if (result) {
    lock_release(sf->lock);
    return EIO;
  }

  sf->offset = ku.uio_offset;
  nread = size - ku.uio_resid;

  if (nread == 0){
    *retval = 0; //EOF
    kfree(kbuf);
    lock_release(sf->lock);
    return 0;
  }
  

  err = copyout(kbuf,buf_ptr,nread);
  if(err){
    kfree(kbuf);
    lock_release(sf->lock);
    return EFAULT;
  }

  kfree(kbuf);
  lock_release(sf->lock);

  *retval = nread;
  return 0;
}


static int
file_write(int fd, userptr_t buf_ptr, size_t size, int * retval) {
  struct iovec iov;
  struct uio ku;
  int result, nwrite;
  struct vnode *vn;
  struct systemFileTable *sf;
  void *kbuf;

  if (fd<0||fd>OPEN_MAX) 
    return EBADF;

  if(buf_ptr == NULL)
    return EFAULT;
    

  sf = curproc->openFileTable[fd];
  if (sf==NULL || sf->mode_open == O_RDONLY) return EBADF;

  vn = sf->vn;
  if (vn==NULL) return EBADF;

  lock_acquire(sf->lock);

  kbuf = kmalloc(size);
  copyin(buf_ptr,kbuf,size);
  uio_kinit(&iov, &ku, kbuf, size, sf->offset, UIO_WRITE);
  result = VOP_WRITE(vn, &ku);
  if (result) {
    lock_release(sf->lock);
    return EFBIG;
  }

  kfree(kbuf);
  sf->offset = ku.uio_offset;
  nwrite = size - ku.uio_resid;

  lock_release(sf->lock);
  *retval = nwrite;
  return 0;
}
#endif


int
sys_write(int fd, userptr_t buf_ptr, size_t size, int * retval)
{
  #if OPT_FILESYSTEM
    return file_write(fd, buf_ptr, size, retval);
  #else
    int i;
    char *p = (char *)buf_ptr;
    if(fd==STDOUT_FILENO || fd==STDERR_FILENO){
      for (i=0; i<(int)size; i++) {
              putch(p[i]);
      }
      return (int)size;
    }
    kprintf("sys_write supported only to stdout and stderr\n");
    return -1;
  #endif

  return (int)-1; //non arriva mai qua
}

int
sys_read(int fd, userptr_t buf_ptr, size_t size, int* retval){
    #if OPT_FILESYSTEM
      return file_read(fd, buf_ptr, size, retval);
    #else
      int i;
      char *p = (char *)buf_ptr;
      if (fd!=STDIN_FILENO) {
        for (i=0; i<(int)size; i++) {
          p[i] = getch();
          if (p[i] < 0) 
            return i;
        }
      }
      kprintf("sys_read supported only to stdin\n");
      return -1;
    #endif

  return (int)-1; //non dovrebbe arrivare qua
}

#if OPT_FILESYSTEM

int sys_open(userptr_t path, int openflag, mode_t mode, int *errp){
  int result;
  struct vnode * v;
  struct systemFileTable *st = NULL;
  int flag = openflag & O_ACCMODE;

  if(path == NULL){
    *errp = EFAULT;
    return -1;
  }

  if (flag != O_RDONLY && flag != O_WRONLY && flag != O_RDWR){
    *errp = EINVAL;
    return -1;
  }
  
  result = vfs_open((char *) path, openflag, mode, &v);

  if(result){
    *errp = ENOENT;
    return -1;
  }

  //cerchiamo in systemFileTable
  //va ad inserire il vnode nella systable alla prima posizione libera
  
  for(int i = 0; i < SYSTEM_OPEN_MAX; i++){
    if(sysTable[i].vn == NULL){
      sysTable[i].vn = v;
      st = &sysTable[i];
      break;
    }
  }
  
  if(st == NULL){
    *errp = ENFILE;
    vfs_close(v);
    return -1;
  }

  //search in File Table
  //partiamo dal primo indice disponibile come fd
  for(int i = 3; i < OPEN_MAX; i++){
    if(curproc->openFileTable[i] == NULL){
      curproc->openFileTable[i] = st; //openFileTable è un puntatore alla SystemFileTable 
      st->offset = 0;
      st->mode_open = openflag;
      st->count_refs = 1;
      st->lock = lock_create("FILE_LOCK");
      return i; //questo i diventa identificatore del file per il processo
    }
  }

  //se arriviamo qua non ci sono entry libere
  st->vn = NULL;
  *errp = EMFILE;
  vfs_close(v);
  return -1;
}

int sys_close(int fd){
  if(fd < 0 || fd > OPEN_MAX) return EBADF;


  struct systemFileTable * sf = curproc->openFileTable[fd];
  if(sf == NULL) return EBADF;


  struct vnode * vn = sf->vn;

  if(vn == NULL) return EBADF;

  lock_acquire(sf->lock);

  sf->count_refs--;

  if(sf->count_refs == 0){
    vfs_close(vn);
    sf->vn=NULL;
  }

  curproc->openFileTable[fd] = NULL;

  lock_release(sf->lock);

  return 0;
}
#endif

#if OPT_SHELL
int sys_lseek(int fd, off_t pos, int whence, int32_t * ret_low, int32_t * ret_up){
  KASSERT(curproc != NULL);
  off_t retval = -1;

  //constrolliamo che non facciamo lseek sul stdinput, output e error
  if(fd <= STDERR_FILENO || fd > OPEN_MAX) return EBADF;

  //con SEEK_SET e SEEK_END non si possono dare valori negativi
  if(pos < 0 && whence != SEEK_CUR){
    return EINVAL; 
  }

  struct systemFileTable * sf = curproc->openFileTable[fd];
  if(sf == NULL) return EBADF;

  struct vnode * vn = sf->vn;
  if(vn == NULL) return EBADF;
  

  /* CHECKING IF IS A FILE */
  if (!VOP_ISSEEKABLE(vn)) {
      return ESPIPE;  // fd refers to an object which does not support seeking.
  }

  //Seek positions beyond EOF are legal (os161 man)
  lock_acquire(sf->lock);
  retval = sf->offset;
  struct stat info;
  int err = 0;
  switch(whence){
    case SEEK_SET:
      retval = pos;
      break;
    case SEEK_CUR:
      retval = sf->offset + pos;
      break;
    case SEEK_END:
      err = VOP_STAT(vn, &info);
      if (err) {
          lock_release(sf->lock);
          return err;
      }

      //offset diventerebbe negativo
      /*if (pos > info.st_size) {
        lock_release(sf->lock);
        return EINVAL;
      }*/

      retval = info.st_size + pos;
      break;
    
    default:
      lock_release(sf->lock);
      return EINVAL;
      break;
  }

  sf->offset = retval;
  *ret_low = (int32_t)(retval >> 32);
  *ret_up = (int32_t) (retval & 0x00000000ffffffff);
  lock_release(sf->lock);
  return 0;
}

//esegue un clone del file descriptor
int sys_dup2(int oldfd, int newfd){
  if(oldfd < 0 || oldfd >= OPEN_MAX) return EBADF;
  
  if(newfd < 0 || newfd >= OPEN_MAX) return EBADF;

  if(oldfd == newfd){
    return oldfd;
  }

  struct systemFileTable * sf = curproc->openFileTable[oldfd];
  if(sf == NULL) return EBADF;

  struct vnode * vn = sf->vn;
  if(vn == NULL) return EBADF;
  
  //chiusura file di newdf se aperto
  if(curproc->openFileTable[newfd]->vn != NULL){
    sys_close(newfd);
  }

  lock_acquire(curproc->openFileTable[oldfd]->lock);
  curproc->openFileTable[newfd] = curproc->openFileTable[oldfd];
  curproc->openFileTable[oldfd]->count_refs++;
  lock_release(curproc->openFileTable[oldfd]->lock);

  return newfd;
}

int sys_chdir(const char *pathname){
  //usiamo la funzione di vfs chiamata vfs_setcurdir

  KASSERT(curproc != NULL);

  if(pathname == NULL) return EFAULT;

  int err = -1;

  char *kbuf = (char *) kmalloc(PATH_MAX * sizeof(char));
    if (kbuf == NULL) {
        return ENOMEM;
    }

  //p_cwd è un vnode, dobbiamo aprire la cartella
  struct vnode * vn = NULL;
  err = copyinstr((userptr_t)pathname, kbuf, sizeof( kbuf ), NULL );

  if(err){
    return EFAULT;
  }
  err = vfs_open((char *) kbuf, O_RDONLY, 0, &vn);
 
  if(err){
    return err;
  }
  
  err = vfs_setcurdir(vn); //already done os161

  if(err){
    vfs_close(vn);
    return err;
  }
  vfs_close(vn);

  return 0; //success
}

int sys__getcwd(char *buf, size_t buflen, int * retval){
  KASSERT(curproc != NULL);
  KASSERT(curthread != NULL); //vfs_getcwd lavora con i thread

  if(buf == NULL){
    return EFAULT;
  }
  
  struct uio u;
  struct iovec iov;

  uio_kinit(
		&iov,
		&u,
		(userptr_t)buf,
		buflen,
		0,
		UIO_READ
	);

  u.uio_space = curthread->t_proc->p_addrspace;
	u.uio_segflg = UIO_USERSPACE;


  /* READING CURRENT DIRECTORY WITH VFS UTILITIES */
  int err = vfs_getcwd(&u);
  if(err){
    return err;
  }

  *retval = buflen - u.uio_resid;
  return 0; 
}
#endif