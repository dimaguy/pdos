//PMGEN    JOB CLASS=C,REGION=0K
//*
//RUNPM    PROC PMPREF='PDMAKE'
//PDMAKE   EXEC PGM=PDMAKE,PARM='dd:in'
//STEPLIB  DD DSN=&PMPREF..LINKLIB,DISP=SHR
//SYSIN    DD DUMMY
//IN       DD DUMMY
//SYSPRINT DD SYSOUT=*
//SYSTERM  DD SYSOUT=*
//         PEND
//*
//CLEAN    PROC PMPREF='PDMAKE'
//DELETE   EXEC PGM=IEFBR14
//DD1      DD DSN=&PMPREF..ALLZIPS,DISP=(MOD,DELETE),
//         UNIT=SYSDA,SPACE=(TRK,(0))
//         PEND
//*
//S1       EXEC RUNPM
//IN       DD *
line 1
undivert(dd:in2)dnl
line 3
/*
//IN2      DD *
line 2
/*
//S2       EXEC CLEAN
//
